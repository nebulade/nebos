/* Kernel heap allocator: power-of-two segregated free lists over mm.c. */

#include "kmalloc.h"
#include "mm.h"

#define KMALLOC_MIN     16      /* smallest block (bytes) */
#define KMALLOC_CLASSES 9       /* 16, 32, ..., 4096 (log2(4096)-log2(16)+1) */

/* Block header: when allocated, first word = size; when free, first word =
 * the free-list link.  The returned pointer is block + sizeof(header). */
union km_block {
	struct { unsigned int size; } h;
	union km_block *next;
};

static union km_block *free_lists[KMALLOC_CLASSES];

static inline unsigned int round_up_pow2(unsigned int n)
{
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

/* size is a power of two in [KMALLOC_MIN, PAGE_SIZE]. */
static inline unsigned int size_class_of(unsigned int size)
{
	unsigned int c = 0;
	while (size > KMALLOC_MIN) {
		size >>= 1;
		c++;
	}
	return c;
}

void kmalloc_init(void)
{
	unsigned int i;
	for (i = 0; i < KMALLOC_CLASSES; i++)
		free_lists[i] = 0;
}

void *kmalloc(unsigned int size)
{
	unsigned int total, cls, off;
	union km_block *b;
	void *page;

	if (size == 0)
		size = 1;

	/* Round up (header + payload) to a power of two. */
	total = round_up_pow2(size + sizeof(unsigned int));
	if (total < KMALLOC_MIN)
		total = KMALLOC_MIN;

	/* Large allocation: whole contiguous pages. */
	if (total > PAGE_SIZE) {
		unsigned int pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
		page = (void *)mm_alloc_pages(pages);
		if (!page)
			return 0;
		((union km_block *)page)->h.size = total;
		return (void *)((unsigned int)page + sizeof(unsigned int));
	}

	cls = size_class_of(total);
	b = free_lists[cls];
	if (!b) {
		/* Refill this size class from a fresh page. */
		page = (void *)mm_alloc_page();
		if (!page)
			return 0;
		for (off = 0; off + total <= PAGE_SIZE; off += total) {
			union km_block *blk =
				(union km_block *)((unsigned int)page + off);
			blk->next = free_lists[cls];
			free_lists[cls] = blk;
		}
		b = free_lists[cls];
	}

	free_lists[cls] = b->next;
	b->h.size = total;
	return (void *)((unsigned int)b + sizeof(unsigned int));
}

void kfree(void *ptr)
{
	union km_block *b;
	unsigned int size, cls;

	if (!ptr)
		return;

	b = (union km_block *)((unsigned int)ptr - sizeof(unsigned int));
	size = b->h.size;

	if (size > PAGE_SIZE) {
		unsigned int pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
		unsigned int addr = (unsigned int)b;
		unsigned int i;
		for (i = 0; i < pages; i++)
			mm_free_page(addr + i * PAGE_SIZE);
		return;
	}

	cls = size_class_of(size);
	b->next = free_lists[cls];
	free_lists[cls] = b;
}
