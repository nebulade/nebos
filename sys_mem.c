/* Per-process user heap: see sys_mem.h. */

#include "sys_mem.h"
#include "mm.h"
#include "paging.h"
#include "process.h"

static void invlpg_va(unsigned int va)
{
	__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static void zero_page(unsigned int phys)
{
	unsigned int *p = (unsigned int *)phys;
	unsigned int i;

	for (i = 0; i < PAGE_SIZE / 4; i++)
		p[i] = 0;
}

/* Map new pages [from, to) of the arena.  On failure release only the pages
 * mapped by this call; the caller's existing arena is untouched. */
static int map_pages(struct process *p, unsigned int from, unsigned int to)
{
	unsigned int i;

	for (i = from; i < to; i++) {
		unsigned int phys = mm_alloc_page();
		unsigned int va = USER_HEAP_BASE + i * PAGE_SIZE;

		if (!phys)
			break;
		zero_page(phys);
		if (!paging_map_page(p->cr3, va, phys,
				     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
			mm_free_page(phys);
			break;
		}
		invlpg_va(va);
	}
	if (i < to) {
		if (i > from)
			paging_release_user_range(p->cr3,
						  USER_HEAP_BASE + from * PAGE_SIZE,
						  (i - from) * PAGE_SIZE, 1);
		return -1;
	}
	return 0;
}

int sys_mem_alloc(unsigned int bytes)
{
	struct process *p = current_proc();
	unsigned int pages;

	if (!p || !p->cr3 || bytes == 0 || p->heap_pages != 0)
		return -1;
	if (bytes > USER_HEAP_MAX)
		return -1;
	pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	if (pages == 0 || pages > USER_HEAP_MAX / PAGE_SIZE)
		return -1;
	if (map_pages(p, 0, pages))
		return -1;
	p->heap_pages = pages;
	return (int)USER_HEAP_BASE;
}

int sys_mem_grow(unsigned int bytes)
{
	struct process *p = current_proc();
	unsigned int pages;

	if (!p || !p->cr3 || bytes == 0 || bytes > USER_HEAP_MAX)
		return -1;
	pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	if (pages <= p->heap_pages)
		return 0;              /* shrinking is deliberately unsupported */
	if (pages > USER_HEAP_MAX / PAGE_SIZE)
		return -1;
	if (map_pages(p, p->heap_pages, pages))
		return -1;
	p->heap_pages = pages;
	return 0;
}

int sys_mem_free(void)
{
	struct process *p = current_proc();

	if (!p || !p->cr3 || !p->heap_pages)
		return -1;
	paging_release_user_range(p->cr3, USER_HEAP_BASE,
				  p->heap_pages * PAGE_SIZE, 1);
	p->heap_pages = 0;
	return 0;
}

void sys_mem_release(struct process *p)
{
	if (!p || !p->cr3 || !p->heap_pages)
		return;
	paging_release_user_range(p->cr3, USER_HEAP_BASE,
				  p->heap_pages * PAGE_SIZE, 1);
	p->heap_pages = 0;
}
