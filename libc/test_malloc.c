/* Host stress test for libc/malloc.c.
 *
 * The allocator is compiled with the malloc/free/... names renamed to
 * libc_*, so the host libc stays untouched and the test can use rand/write
 * without recursion.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *libc_malloc(size_t size);
void libc_free(void *ptr);
void *libc_calloc(size_t nmemb, size_t size);
void *libc_realloc(void *ptr, size_t size);
void libc_heap_reset(void);
unsigned int host_mem_mapped(void);

#define SLOTS       96
#define LIVE_CAP    (24u * 1024u * 1024u)

struct slot {
	unsigned char *p;
	size_t n;
	unsigned char pat;
};

static struct slot slots[SLOTS];
static unsigned int live_bytes;
static unsigned int pattern_seed;
static int failures;

static void out(const char *s)
{
	(void)!write(1, s, strlen(s));
}

static void out_num(unsigned long n)
{
	char buf[24];
	char *p = buf + sizeof(buf);

	*--p = '\0';
	if (n == 0)
		*--p = '0';
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	out(p);
}

static void fail(const char *msg)
{
	out("FAIL: ");
	out(msg);
	out("\n");
	failures++;
}

static int ranges_overlap(const void *a, size_t an, const void *b, size_t bn)
{
	uintptr_t pa = (uintptr_t)a;
	uintptr_t pb = (uintptr_t)b;

	return pa < pb + bn && pb < pa + an;
}

static int overlaps_live(const void *p, size_t n, int skip)
{
	int i;

	for (i = 0; i < SLOTS; i++) {
		if (i == skip || !slots[i].p)
			continue;
		if (ranges_overlap(p, n, slots[i].p, slots[i].n))
			return 1;
	}
	return 0;
}

static void fill(unsigned char *p, size_t n, unsigned char pat)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = pat;
}

static int check_fill(const unsigned char *p, size_t n, unsigned char pat)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] != pat)
			return 0;
	}
	return 1;
}

static size_t pick_size(void)
{
	unsigned int r = (unsigned int)rand();

	if ((r & 63u) == 0)
		return 1 + (size_t)(rand() % (4 * 1024 * 1024));
	if ((r & 7u) == 0)
		return 1 + (size_t)(rand() % (256 * 1024));
	return 1 + (size_t)(rand() % 8192);
}

static int find_free_slot(void)
{
	int i;

	for (i = 0; i < SLOTS; i++) {
		if (!slots[i].p)
			return i;
	}
	return -1;
}

static int find_live_slot(void)
{
	int i;

	for (i = 0; i < SLOTS; i++) {
		if (slots[i].p)
			return i;
	}
	return -1;
}

static void free_slot(int i)
{
	if (i < 0 || !slots[i].p)
		return;
	if (!check_fill(slots[i].p, slots[i].n, slots[i].pat))
		fail("pattern changed before free");
	libc_free(slots[i].p);
	live_bytes -= (unsigned int)slots[i].n;
	slots[i].p = 0;
	slots[i].n = 0;
}

static void test_basics(void)
{
	unsigned char *p;
	unsigned char *q;
	unsigned char *r;
	size_t i;

	if (libc_malloc(0) != 0)
		fail("malloc(0) is not NULL");
	libc_free(0);

	p = (unsigned char *)libc_malloc(100);
	if (!p) {
		fail("malloc(100) failed");
		return;
	}
	if ((uintptr_t)p & 15u)
		fail("malloc is not 16-byte aligned");
	fill(p, 100, 0x5A);
	if (!check_fill(p, 100, 0x5A))
		fail("malloc contents changed");
	libc_free(p);

	p = (unsigned char *)libc_calloc(1024, 4);
	if (!p) {
		fail("calloc failed");
	} else {
		for (i = 0; i < 4096; i++) {
			if (p[i] != 0) {
				fail("calloc returned non-zero memory");
				break;
			}
		}
		libc_free(p);
	}

	p = (unsigned char *)libc_realloc(0, 100);
	if (!p) {
		fail("realloc(NULL, 100) failed");
	} else {
		fill(p, 100, 0x33);
		if (libc_realloc(p, 0) != 0)
			fail("realloc(ptr, 0) did not return NULL");
	}

	p = (unsigned char *)libc_malloc(1024);
	q = (unsigned char *)libc_malloc(2048);
	if (!p || !q) {
		fail("two small allocations failed");
		libc_free(p);
		libc_free(q);
	} else {
		fill(p, 1024, 0x11);
		fill(q, 2048, 0x22);
		if (overlaps_live(p, 1024, -1) ||
		    overlaps_live(q, 2048, -1))
			fail("small allocations overlap");
		if (!check_fill(p, 1024, 0x11) ||
		    !check_fill(q, 2048, 0x22))
			fail("small allocation contents changed");
		r = (unsigned char *)libc_realloc(p, 8192);
		if (!r) {
			fail("realloc grow failed");
			libc_free(p);
		} else {
			if (!check_fill(r, 1024, 0x11))
				fail("realloc lost the prefix");
			fill(r, 8192, 0x44);
			libc_free(r);
		}
		libc_free(q);
	}

	if (libc_calloc((size_t)-1, 2) != 0)
		fail("calloc overflow did not fail");
}

static void test_random(void)
{
	unsigned int op;
	int i;

	for (op = 0; op < 4000; op++) {
		int fslot = find_free_slot();
		int live = find_live_slot();
		unsigned int r = (unsigned int)rand();

		if (live < 0 || (fslot >= 0 && (r & 3u) != 0)) {
			size_t n = pick_size();
			unsigned char *p;
			unsigned char pat;

			if (live_bytes + (unsigned int)n > LIVE_CAP &&
			    live >= 0) {
				free_slot(live);
				fslot = find_free_slot();
			}
			if (fslot < 0)
				continue;
			p = (unsigned char *)libc_malloc(n);
			if (!p) {
				fail("random malloc failed");
				continue;
			}
			if ((uintptr_t)p & 15u)
				fail("random malloc is not 16-byte aligned");
			if (overlaps_live(p, n, -1))
				fail("random allocations overlap");
			pat = (unsigned char)(0x30 + (pattern_seed++ & 0x3F));
			fill(p, n, pat);
			slots[fslot].p = p;
			slots[fslot].n = n;
			slots[fslot].pat = pat;
			live_bytes += (unsigned int)n;
		} else {
			free_slot(live);
		}
	}

	/* A realloc pass over the surviving blocks. */
	for (i = 0; i < SLOTS; i++) {
		size_t n;
		size_t old;
		unsigned char *p;
		unsigned char *q;
		unsigned char pat;

		if (!slots[i].p)
			continue;
		old = slots[i].n;
		n = pick_size();
		if (n > old && live_bytes + (unsigned int)(n - old) > LIVE_CAP)
			n = old;
		p = slots[i].p;
		q = (unsigned char *)libc_realloc(p, n);
		if (!q) {
			fail("random realloc failed");
			continue;
		}
		if (overlaps_live(q, n, i))
			fail("realloc overlaps another block");
		if (!check_fill(q, old < n ? old : n, slots[i].pat))
			fail("realloc lost the prefix");
		pat = (unsigned char)(0x80 + (pattern_seed++ & 0x3F));
		fill(q, n, pat);
		live_bytes = live_bytes - (unsigned int)old + (unsigned int)n;
		slots[i].p = q;
		slots[i].n = n;
		slots[i].pat = pat;
	}
}

static void free_all(void)
{
	int i;

	for (i = 0; i < SLOTS; i++)
		free_slot(i);
	if (live_bytes != 0)
		fail("live byte count is not zero");
}

static void test_large(void)
{
	unsigned char *p;
	unsigned char *q;
	size_t n = 8u * 1024u * 1024u;

	p = (unsigned char *)libc_malloc(n);
	if (!p) {
		fail("8 MB allocation failed");
	} else {
		fill(p, n, 0x6C);
		if (!check_fill(p, n, 0x6C))
			fail("8 MB pattern changed");
		libc_free(p);
	}

	p = (unsigned char *)libc_malloc(1024u * 1024u);
	if (!p) {
		fail("1 MB allocation failed");
		return;
	}
	fill(p, 1024u * 1024u, 0x21);
	q = (unsigned char *)libc_realloc(p, 4u * 1024u * 1024u);
	if (!q) {
		fail("1 MB -> 4 MB realloc failed");
		libc_free(p);
		return;
	}
	if (!check_fill(q, 1024u * 1024u, 0x21))
		fail("large realloc lost the prefix");
	fill(q, 4u * 1024u * 1024u, 0x42);
	libc_free(q);
}

int main(void)
{
	unsigned int seed;

	for (seed = 1; seed <= 8; seed++) {
		srand(seed);
		pattern_seed = 0;
		test_basics();
		test_random();
		free_all();

		libc_heap_reset();
		if (host_mem_mapped() != 0)
			fail("heap_reset did not unmap the arena");
	}

	test_large();
	libc_heap_reset();
	if (host_mem_mapped() != 0)
		fail("second heap_reset did not unmap the arena");

	if (failures) {
		out("malloc test: ");
		out_num((unsigned long)failures);
		out(" failure(s)\n");
		return 1;
	}
	out("malloc test: OK\n");
	return 0;
}
