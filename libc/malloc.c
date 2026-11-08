/* Ring-3 heap allocator: boundary-tag blocks with first-fit free lists.
 *
 * The heap lives in the process's single SYS_MEM_* arena at USER_HEAP_BASE.
 * It is mapped lazily (64 KB at first use) and grows in page-granular steps
 * with a small slack, so a large image arena does not reserve the whole
 * 32 MB window.  heap_reset() unmaps everything for page-scoped programs.
 *
 * There are no threads in ring 3 and the arena is private to the process, so
 * no locking is needed.  malloc() returns 16-byte aligned memory; calloc()
 * zeroes, malloc() does not (grown pages are not zeroed by the kernel).
 *
 * Block layout:
 *
 *   +0   size      total block size, low bit = free
 *   +4   prev_size size of the preceding physical block (0 at the base)
 *   +8   magic     ALLOC_MAGIC or FREE_MAGIC (double-free guard)
 *   +12  reserved
 *   +16  payload
 *
 * Free blocks keep their next/prev links in the first 8 payload bytes.  The
 * minimum block is therefore 32 bytes.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ucmd_heap.h>

#define HEAP_INIT_BYTES  (64u * 1024u)
#define HEAP_MAX_BYTES   (32u * 1024u * 1024u)
#define HEAP_GROW_SLACK  (64u * 1024u)
#define HEAP_ALIGN       16u
#define HDR_SIZE         16u
#define MIN_BLOCK        32u

#define FREE_BIT         1u
#define ALLOC_MAGIC      0x4B424C4Eu     /* "NBLK" */
#define FREE_MAGIC       0x45455246u     /* "FREE" */

struct blk {
	unsigned int size;
	unsigned int prev_size;
	unsigned int magic;
	unsigned int reserved;
};

struct freelink {
	struct blk *next;
	struct blk *prev;
};

/* Implemented by libc/ucmd_mem.c (target) or the host test backend. */
void *__heap_map(unsigned int bytes);
int __heap_grow(unsigned int total_bytes);
void __heap_unmap(void);

static unsigned char *heap_base;
static unsigned int heap_bytes;     /* mapped bytes */
static unsigned int heap_end;       /* end of the top block */
static struct blk *top_blk;
static struct blk *free_head;
static int heap_ready;

static unsigned int align_up(unsigned int n)
{
	return (n + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
}

static unsigned int blk_size(const struct blk *b)
{
	return b->size & ~FREE_BIT;
}

static int blk_is_free(const struct blk *b)
{
	return (b->size & FREE_BIT) != 0;
}

static struct blk *blk_at(unsigned int off)
{
	return (struct blk *)(void *)(heap_base + off);
}

static unsigned int blk_off(const struct blk *b)
{
	return (unsigned int)((const unsigned char *)b - heap_base);
}

static struct blk *blk_next(struct blk *b)
{
	unsigned int off = blk_off(b) + blk_size(b);

	if (off >= heap_end)
		return 0;
	return blk_at(off);
}

static struct freelink *blk_link(struct blk *b)
{
	return (struct freelink *)(void *)((unsigned char *)b + HDR_SIZE);
}

static void free_list_insert(struct blk *b)
{
	struct freelink *f = blk_link(b);

	f->prev = 0;
	f->next = free_head;
	if (free_head)
		blk_link(free_head)->prev = b;
	free_head = b;
}

static void free_list_remove(struct blk *b)
{
	struct freelink *f = blk_link(b);

	if (f->prev)
		blk_link(f->prev)->next = f->next;
	else
		free_head = f->next;
	if (f->next)
		blk_link(f->next)->prev = f->prev;
}

/* Split an allocated block so that it is exactly `need` bytes.  If the
 * remainder is too small to form a block, keep the whole block. */
static void split_allocated(struct blk *b, unsigned int need)
{
	unsigned int old = blk_size(b);
	struct blk *rest;
	struct blk *after;

	if (old - need >= MIN_BLOCK) {
		rest = (struct blk *)(void *)((unsigned char *)b + need);
		rest->size = (old - need) | FREE_BIT;
		rest->prev_size = need;
		rest->magic = FREE_MAGIC;
		rest->reserved = 0;

		b->size = need;
		b->magic = ALLOC_MAGIC;

		after = blk_next(rest);
		if (after)
			after->prev_size = blk_size(rest);
		if (b == top_blk)
			top_blk = rest;
		free_list_insert(rest);
	} else {
		b->size = old;
		b->magic = ALLOC_MAGIC;
	}
}

/* Free `b`, merging it with a free predecessor and successor when possible. */
static void free_block(struct blk *b)
{
	struct blk *prev;
	struct blk *next;
	struct blk *after;
	unsigned int merged;

	b->size |= FREE_BIT;
	b->magic = FREE_MAGIC;

	if (b->prev_size) {
		prev = (struct blk *)(void *)((unsigned char *)b - b->prev_size);
		if (blk_is_free(prev)) {
			free_list_remove(prev);
			merged = blk_size(prev) + blk_size(b);
			prev->size = merged | FREE_BIT;
			prev->magic = FREE_MAGIC;
			if (b == top_blk)
				top_blk = prev;
			after = blk_next(prev);
			if (after)
				after->prev_size = merged;
			b = prev;
		}
	}

	next = blk_next(b);
	if (next && blk_is_free(next)) {
		free_list_remove(next);
		merged = blk_size(b) + blk_size(next);
		b->size = merged | FREE_BIT;
		b->magic = FREE_MAGIC;
		if (next == top_blk)
			top_blk = b;
		after = blk_next(b);
		if (after)
			after->prev_size = merged;
	}

	free_list_insert(b);
}

/* Extend the mapped arena so that the top block ends at or past
 * `need_total`, then merge the new region into the top block. */
static int heap_extend(unsigned int need_total)
{
	unsigned int old = heap_bytes;
	unsigned int target;
	struct blk *b;

	if (need_total > HEAP_MAX_BYTES)
		return -1;

	target = need_total;
	if (target < old + HEAP_GROW_SLACK)
		target = old + HEAP_GROW_SLACK;
	if (target > HEAP_MAX_BYTES)
		target = HEAP_MAX_BYTES;
	target = (target + 4095u) & ~4095u;
	if (target > HEAP_MAX_BYTES)
		target = HEAP_MAX_BYTES;
	if (target < need_total || target <= old)
		return -1;

	if (__heap_grow(target) != 0)
		return -1;
	heap_bytes = target;

	if (top_blk && blk_is_free(top_blk)) {
		/* The top block already reaches `old`; extend it in place. */
		free_list_remove(top_blk);
		top_blk->size = (blk_size(top_blk) + (target - old)) | FREE_BIT;
		top_blk->magic = FREE_MAGIC;
		free_list_insert(top_blk);
	} else {
		b = blk_at(old);
		b->size = (target - old) | FREE_BIT;
		b->prev_size = top_blk ? blk_size(top_blk) : 0;
		b->magic = FREE_MAGIC;
		b->reserved = 0;
		free_list_insert(b);
		top_blk = b;
	}
	heap_end = target;
	return 0;
}

static int heap_init(void)
{
	struct blk *b;
	void *base;

	if (heap_ready)
		return 0;

	base = __heap_map(HEAP_INIT_BYTES);
	if (!base)
		return -1;
	if ((unsigned long)base & (HEAP_ALIGN - 1u)) {
		__heap_unmap();
		return -1;
	}

	heap_base = (unsigned char *)base;
	heap_bytes = HEAP_INIT_BYTES;
	heap_end = HEAP_INIT_BYTES;
	free_head = 0;

	b = blk_at(0);
	b->size = HEAP_INIT_BYTES | FREE_BIT;
	b->prev_size = 0;
	b->magic = FREE_MAGIC;
	b->reserved = 0;
	top_blk = b;
	free_list_insert(b);
	heap_ready = 1;
	return 0;
}

static void *alloc_block(unsigned int need)
{
	struct blk *b = free_head;
	void *payload;

	while (b && blk_size(b) < need)
		b = blk_link(b)->next;

	if (!b) {
		if (heap_extend(heap_end + need) != 0)
			return 0;
		b = free_head;
		while (b && blk_size(b) < need)
			b = blk_link(b)->next;
		if (!b)
			return 0;
	}

	free_list_remove(b);
	payload = (void *)((unsigned char *)b + HDR_SIZE);
	split_allocated(b, need);
	return payload;
}

void *malloc(size_t size)
{
	unsigned int need;

	if (size == 0 || size > HEAP_MAX_BYTES)
		return 0;
	if (size > (size_t)(HEAP_MAX_BYTES - HDR_SIZE))
		return 0;
	if (!heap_ready && heap_init() != 0)
		return 0;

	need = align_up(HDR_SIZE + (unsigned int)size);
	if (need < MIN_BLOCK)
		need = MIN_BLOCK;
	if (need > HEAP_MAX_BYTES)
		return 0;
	return alloc_block(need);
}

static struct blk *block_from_ptr(void *ptr)
{
	unsigned char *p = (unsigned char *)ptr;
	struct blk *b;
	unsigned int size;

	if (!heap_ready || !ptr)
		return 0;
	if (p < heap_base + HDR_SIZE || p >= heap_base + heap_bytes)
		return 0;

	b = (struct blk *)(void *)(p - HDR_SIZE);
	if (b->magic != ALLOC_MAGIC)
		return 0;
	size = blk_size(b);
	if (size < MIN_BLOCK || (unsigned char *)b + size > heap_base + heap_bytes)
		return 0;
	return b;
}

void free(void *ptr)
{
	struct blk *b = block_from_ptr(ptr);

	if (b)
		free_block(b);
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *p;

	if (nmemb == 0 || size == 0)
		return 0;
	if (nmemb > (size_t)-1 / size)
		return 0;
	total = nmemb * size;
	if (total > HEAP_MAX_BYTES)
		return 0;

	p = malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}

void *realloc(void *ptr, size_t size)
{
	struct blk *b;
	struct blk *next;
	unsigned int old;
	unsigned int need;
	unsigned int merged;
	unsigned int copy;

	if (!ptr)
		return malloc(size);
	if (size == 0) {
		free(ptr);
		return 0;
	}
	if (size > HEAP_MAX_BYTES ||
	    size > (size_t)(HEAP_MAX_BYTES - HDR_SIZE))
		return 0;

	b = block_from_ptr(ptr);
	if (!b)
		return 0;

	need = align_up(HDR_SIZE + (unsigned int)size);
	if (need < MIN_BLOCK)
		need = MIN_BLOCK;

	old = blk_size(b);
	if (need <= old) {
		split_allocated(b, need);
		return ptr;
	}

	next = blk_next(b);
	if (next && blk_is_free(next) && old + blk_size(next) >= need) {
		free_list_remove(next);
		merged = old + blk_size(next);
		b->size = merged;
		b->magic = ALLOC_MAGIC;
		if (next == top_blk) {
			top_blk = b;
		} else {
			struct blk *after = blk_next(b);

			if (after)
				after->prev_size = merged;
		}
		split_allocated(b, need);
		return ptr;
	}

	{
		void *np = malloc(size);

		if (!np)
			return 0;
		copy = old - HDR_SIZE;
		if (copy > size)
			copy = (unsigned int)size;
		memcpy(np, ptr, copy);
		free(ptr);
		return np;
	}
}

void heap_reset(void)
{
	if (!heap_ready)
		return;
	__heap_unmap();
	heap_base = 0;
	heap_bytes = 0;
	heap_end = 0;
	top_blk = 0;
	free_head = 0;
	heap_ready = 0;
}
