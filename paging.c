/* Paging: identity-map RAM + the VESA LFB, enable CR0.PG (4 MB pages).
 *
 * This is the "paging on but flat" stage: every physical address is mapped
 * 1:1, so all existing code (kernel, stacks, DMA, UHCI, VGA shadow) keeps
 * working unchanged.  Per-process page directories come later (Phase 3b/4).
 *
 * Uses 4 MB pages via CR4.PSE (supported since the original Pentium), so the
 * whole map is a single 4 KB page directory.
 */

#include "paging.h"
#include "mm.h"
#include "mtrr.h"
#include "serial.h"
#include "vga.h"

/* Page-directory entry flags (4 MB page). */
#define PDE_PRESENT  0x001
#define PDE_WRITABLE 0x002
#define PDE_USER     0x004
#define PDE_PWT      0x008
#define PDE_PCD      0x010
#define PDE_PS       0x080   /* 4 MB page */

/* How much of the LFB to map.  16 MB covers any mode up to 1280x1024x32
 * (~5 MB) with margin, and is independent of the BIOS's VRAM report. */
#define LFB_MAP_SIZE 0x1000000

static unsigned int *page_dir;

void paging_init(void)
{
	unsigned int i;
	const volatile struct vga_boot_fb_info *fb = VGA_BOOT_FB;
	/* Opaque asm hides the fixed 0x600 pointer from -Warray-bounds. */
	__asm__ volatile ("" : "+r"(fb));

	page_dir = (unsigned int *)mm_alloc_page();
	if (!page_dir) {
		serial_puts(SERIAL_COM1, "paging: out of memory\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	/* Zero the whole page directory.  mm_alloc_page does NOT clear memory, so
	 * on real hardware the high PDEs (indices 32..1023, incl. the user region
	 * at 0x100) would otherwise be garbage.  QEMU zeroes guest RAM, which is
	 * why this only bites on real silicon. */
	for (i = 0; i < 1024; i++)
		page_dir[i] = 0;

	/* Identity-map RAM [0, MM_MAX_PHYS) with 4 MB pages (write-back). */
	for (i = 0; i < MM_MAX_PHYS / 0x400000; i++)
		page_dir[i] = (i << 22) | PDE_PS | PDE_WRITABLE | PDE_PRESENT;

	/* Identity-map the framebuffer (MMIO, uncacheable) so the screen works. */
	if (fb->valid && fb->lfb_base) {
		unsigned int base = fb->lfb_base;
		unsigned int p0 = base >> 22;
		unsigned int p1 = (base + LFB_MAP_SIZE) >> 22;
		for (i = p0; i < p1 && i < 1024; i++)
			page_dir[i] = (i << 22) | PDE_PS | PDE_PCD |
				      PDE_WRITABLE | PDE_PRESENT;
		/* Mark the framebuffer write-combining so VRAM writes batch.
		 * Use the actual framebuffer size (not the whole mapping) so
		 * the WC range cannot swallow unrelated MMIO. */
		{
			unsigned int wc_size = (unsigned int)fb->pitch *
					       fb->height;
			if (wc_size == 0 || wc_size > LFB_MAP_SIZE)
				wc_size = LFB_MAP_SIZE;
			mtrr_set_wc(base, wc_size);
		}
	}

	/* Load CR3, enable PSE, then PG + WP.  The jump flushes the prefetch
	 * queue after the paging flip. */
	__asm__ volatile (
		"movl %0, %%cr3\n\t"
		"movl %%cr4, %%eax\n\t"
		"orl  $0x10, %%eax\n\t"       /* CR4.PSE */
		"movl %%eax, %%cr4\n\t"
		"movl %%cr0, %%eax\n\t"
		"orl  $0x80010000, %%eax\n\t" /* CR0.PG | CR0.WP */
		"movl %%eax, %%cr0\n\t"
		"jmp  1f\n\t"
		"1:\n\t"
		:
		: "r"((unsigned int)page_dir)
		: "eax", "memory");

	serial_puts(SERIAL_COM1, "paging: enabled (identity map)\n");
}

unsigned int paging_current_cr3(void)
{
	return (unsigned int)page_dir;
}

unsigned int paging_new_directory(void)
{
	unsigned int *pd = (unsigned int *)mm_alloc_page();
	unsigned int i;

	if (!pd)
		return 0;
	for (i = 0; i < 1024; i++)
		pd[i] = page_dir[i];
	return (unsigned int)pd;
}

int paging_map_page(unsigned int cr3, unsigned int va, unsigned int phys,
		    unsigned int flags)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int pde = va >> 22;
	unsigned int pte = (va >> 12) & 0x3FF;
	unsigned int *pt;

	if (!(pd[pde] & PDE_PRESENT)) {
		unsigned int pt_phys = mm_alloc_page();
		unsigned int i;
		if (!pt_phys)
			return 0;
		pt = (unsigned int *)pt_phys;
		for (i = 0; i < 1024; i++)
			pt[i] = 0;
		pd[pde] = pt_phys | PDE_USER | PDE_WRITABLE | PDE_PRESENT;
	} else {
		pt = (unsigned int *)(pd[pde] & 0xFFFFF000);
	}

	pt[pte] = (phys & 0xFFFFF000) | flags;
	return 1;
}

void paging_unmap_page(unsigned int cr3, unsigned int va)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int pde = va >> 22;
	unsigned int pte = (va >> 12) & 0x3FF;

	if (!(pd[pde] & PDE_PRESENT))
		return;
	((unsigned int *)(pd[pde] & 0xFFFFF000))[pte] = 0;
}

/* Identity-map an MMIO range uncacheable into one page directory (4 MB pages;
 * the range is rounded up to a whole number of them). */
void paging_map_mmio_cr3(unsigned int cr3, unsigned int phys_base, unsigned int size)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int p0 = phys_base >> 22;
	unsigned int p1 = (phys_base + size + 0x3FFFFFu) >> 22;
	unsigned int i;

	for (i = p0; i < p1 && i < 1024; i++)
		pd[i] = (i << 22) | PDE_PS | PDE_PCD |
			PDE_WRITABLE | PDE_PRESENT;
}

void paging_map_mmio(unsigned int phys_base, unsigned int size)
{
	paging_map_mmio_cr3((unsigned int)page_dir, phys_base, size);
}
