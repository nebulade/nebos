/* Paging: identity-map RAM + the VESA LFB and enable CR0.PG (4 MB pages). */
#ifndef PAGING_H
#define PAGING_H

void paging_init(void);
/* Re-apply the framebuffer page types (after GPU BAR mappings that cover it). */
void paging_lfb_refresh(void);
/* 1 when the LFB is mapped for write-combining. */
int paging_lfb_wc(void);

/* Map a physical window identity-mapped with caching enabled (4 MB pages).
 * Used for the stolen-VRAM alias, which is ordinary DRAM; the effective type
 * is whatever the MTRRs say (write-combining when we added an entry). */
void paging_map_ram(unsigned int base, unsigned int size);

/* Physical address of the page directory currently loaded in CR3.  On SMP this
 * is per-CPU; on one CPU it is the running process's directory. */
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

/* Physical address mapped at `va`, or 0 when the page is not present. */
unsigned int paging_page_phys(unsigned int cr3, unsigned int va);

/* Clear one 4 KB page (va) in page directory `cr3` (no-op when the page
 * table is absent). */
void paging_unmap_page(unsigned int cr3, unsigned int va);

/* Release the user mappings in [va, va+size): clear the PTEs, free empty page
 * tables and, when `free_phys` is set, free the mapped physical pages.  With
 * `free_phys` clear the pages belong to someone else (e.g. a mapped window
 * canvas) and only the mapping/page tables are released.  Used by the process
 * reaper; ranges should cover the user-owned mappings only. */
void paging_release_user_range(unsigned int cr3, unsigned int va,
			       unsigned int size, int free_phys);

/* 1 when every page in [va, va+size) is a user-accessible 4 KB mapping in
 * `cr3` (and writable when `write` is set).  Syscall services use this before
 * touching a pointer supplied by ring 3. */
int paging_user_range_ok(unsigned int cr3, unsigned int va,
			 unsigned int size, int write);

/* Identity-map an MMIO range (e.g. a GPU register aperture) uncacheable into
 * the current page directory.  Uses 4 MB pages; the range is rounded up to a
 * whole number of 4 MB pages. */
void paging_map_mmio(unsigned int phys_base, unsigned int size);

/* Same, but into an explicit page directory (used to update per-process
 * directories; see process_map_mmio_all()). */
void paging_map_mmio_cr3(unsigned int cr3, unsigned int phys_base,
			 unsigned int size);

#endif
