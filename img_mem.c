/* Bump allocator over a caller-provided arena (see img.h). */

#include "img.h"

void img_mem_init(struct img_mem *m, void *base, unsigned int size)
{
	m->base = (unsigned char *)base;
	m->size = size;
	m->used = 0;
}

unsigned int img_mem_mark(const struct img_mem *m)
{
	return m->used;
}

int img_mem_rewind(struct img_mem *m, unsigned int mark)
{
	if (mark > m->used)
		return -1;
	m->used = mark;
	return 0;
}

void *img_mem_alloc(struct img_mem *m, unsigned int bytes, unsigned int align)
{
	unsigned int start, end;

	if (!m || !m->base || bytes == 0)
		return 0;
	if (align < 1)
		align = 1;
	if (align & (align - 1))
		return 0;
	start = (m->used + align - 1) & ~(align - 1);
	if (start > m->size || bytes > m->size - start)
		return 0;
	end = start + bytes;
	m->used = end;
	return m->base + start;
}
