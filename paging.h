/* Paging: identity-map RAM + the VESA LFB and enable CR0.PG (4 MB pages). */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);

/* Physical address of the bootstrap (current) page directory. */
unsigned int paging_current_cr3(void);

/* Allocate a fresh page directory that clones the kernel identity map and
 * return its physical address (0 on out-of-memory). */
unsigned int paging_new_directory(void);

/* Page-table entry flags (4 KB pages). */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004

/* Map one 4 KB page (va -> phys) in page directory `cr3`, allocating a page
 * table if needed.  Returns 1 on success, 0 on out-of-memory. */
int paging_map_page(unsigned int cr3, unsigned int va, unsigned int phys,
		    unsigned int flags);

/* Clear one 4 KB page (va) in page directory `cr3` (no-op when the page
 * table is absent). */
void paging_unmap_page(unsigned int cr3, unsigned int va);

/* Identity-map an MMIO range (e.g. a GPU register aperture) uncacheable into
 * the current page directory.  Uses 4 MB pages; the range is rounded up to a
 * whole number of 4 MB pages. */
void paging_map_mmio(unsigned int phys_base, unsigned int size);

/* Same, but into an explicit page directory (used to update per-process
 * directories; see process_map_mmio_all()). */
void paging_map_mmio_cr3(unsigned int cr3, unsigned int phys_base,
			 unsigned int size);

#endif
