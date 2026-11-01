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
/* LFB page-type decision, kept so it can be re-applied after other MMIO
 * mappings (the GPU BARs cover the framebuffer address) have set PCD. */
static unsigned int lfb_wc_base, lfb_wc_size;
static int lfb_wc_ok;

/* Write the LFB PDEs: WC-covered pages cacheable (effective type WC), the
 * rest uncacheable. */
static void lfb_apply_pages(unsigned int *pd)
{
	const volatile struct vga_boot_fb_info *fb = VGA_BOOT_FB;
	unsigned int base, p0, p1, i;

	__asm__ volatile ("" : "+r"(fb));
	if (!fb->valid || !fb->lfb_base)
		return;
	base = fb->lfb_base;
	p0 = base >> 22;
	p1 = (base + LFB_MAP_SIZE) >> 22;
	for (i = p0; i < p1 && i < 1024; i++) {
		unsigned int page = i << 22;
		unsigned int flags = PDE_PS | PDE_WRITABLE | PDE_PRESENT;

		if (!lfb_wc_ok || page < lfb_wc_base ||
		    page + 0x400000 > lfb_wc_base + lfb_wc_size)
			flags |= PDE_PCD;
		pd[i] = page | flags;
	}
}

/* Re-apply the framebuffer mapping; call after any MMIO mapping that may have
 * covered the same addresses (the NVIDIA BAR3 aperture includes the VBE LFB). */
void paging_lfb_refresh(void)
{
	if (!lfb_wc_ok || !page_dir)
		return;
	lfb_apply_pages(page_dir);
	serial_puts(SERIAL_COM1, "paging: LFB write-combining re-applied\n");
}

/* 1 when the LFB is mapped for write-combining (WC MTRR + cacheable pages). */
int paging_lfb_wc(void)
{
	return lfb_wc_ok;
}

void paging_map_ram(unsigned int base, unsigned int size)
{
	unsigned int p0 = base >> 22;
	unsigned int p1 = (base + size + 0x3FFFFFu) >> 22;
	unsigned int i;

	if (!page_dir)
		return;
	for (i = p0; i < p1 && i < 1024; i++)
		page_dir[i] = (i << 22) | PDE_PS | PDE_WRITABLE | PDE_PRESENT;
}

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

	/* Identity-map the framebuffer and make VRAM writes batch.  Use the
	 * actual framebuffer size (not the whole mapping) so the WC range
	 * cannot swallow unrelated MMIO, and install the MTRR *before* choosing
	 * the page type: a PCD=1 page is uncacheable no matter what the MTRRs
	 * say, which made every VRAM dword a separate PCI write (~30 MB/s).
	 * Only the pages fully covered by the WC entry become cacheable (their
	 * effective type is then WC); everything else stays UC. */
	if (fb->valid && fb->lfb_base) {
		unsigned int want = (unsigned int)fb->pitch * fb->height;

		if (want == 0 || want > LFB_MAP_SIZE)
			want = LFB_MAP_SIZE;
		lfb_wc_ok = mtrr_set_wc(fb->lfb_base, want,
					&lfb_wc_base, &lfb_wc_size);
		lfb_apply_pages(page_dir);
		/* Tell the truth about VRAM writes: with cacheable pages the WC
		 * MTRR applies (fast bursts); otherwise every dword is a separate
		 * uncached write. */
		serial_puts(SERIAL_COM1, lfb_wc_ok
			    ? "paging: LFB write-combining\n"
			    : "paging: LFB uncached (no WC MTRR)\n");
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
	unsigned int cr3;

	__asm__ volatile ("movl %%cr3, %0" : "=r"(cr3));
	return cr3 & 0xFFFFF000u;
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

unsigned int paging_page_phys(unsigned int cr3, unsigned int va)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int pde = pd[va >> 22];
	unsigned int pte = (va >> 12) & 0x3FF;

	if (!(pde & PDE_PRESENT) || (pde & PDE_PS))
		return 0;
	{
		unsigned int e = ((unsigned int *)(pde & 0xFFFFF000))[pte];

		return (e & PTE_PRESENT) ? (e & 0xFFFFF000) : 0;
	}
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

int paging_user_range_ok(unsigned int cr3, unsigned int va,
			 unsigned int size, int write)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int end = va + size;
	unsigned int p;

	if (!pd || size == 0 || end < va)
		return 0;
	for (p = va & 0xFFFFF000u; p < end; p += PAGE_SIZE) {
		unsigned int pde = pd[p >> 22];
		unsigned int *pt;
		unsigned int pte;

		if (!(pde & PDE_PRESENT) || (pde & PDE_PS) || !(pde & PDE_USER))
			return 0;
		pt = (unsigned int *)(pde & 0xFFFFF000);
		pte = pt[(p >> 12) & 0x3FF];
		if (!(pte & PTE_PRESENT) || !(pte & PTE_USER))
			return 0;
		if (write && !(pte & PTE_WRITABLE))
			return 0;
	}
	return 1;
}

void paging_release_user_range(unsigned int cr3, unsigned int va,
			       unsigned int size, int free_phys)
{
	unsigned int *pd = (unsigned int *)cr3;
	unsigned int end = va + size;
	unsigned int first, last, i, j;

	if (!pd || size == 0 || end < va)
		return;
	first = va >> 22;
	last = (end - 1) >> 22;

	for (i = first; i <= last && i < 1024; i++) {
		unsigned int pde = pd[i];
		unsigned int *pt;
		int empty = 1;

		/* Kernel identity mappings are 4 MB pages and must never be
		 * touched here. */
		if (!(pde & PDE_PRESENT) || (pde & PDE_PS))
			continue;
		pt = (unsigned int *)(pde & 0xFFFFF000);

		for (j = 0; j < 1024; j++) {
			unsigned int pva = (i << 22) | (j << 12);

			if (pva < va || pva >= end)
				continue;
			if (pt[j] & PTE_PRESENT) {
				/* Only user pages are owned by the process; a
				 * supervisor mapping in this range is left for
				 * its owner. */
				if (free_phys && (pt[j] & PTE_USER))
					mm_free_page(pt[j] & 0xFFFFF000);
				pt[j] = 0;
			}
		}

		for (j = 0; j < 1024; j++)
			if (pt[j] & PTE_PRESENT) {
				empty = 0;
				break;
			}
		if (empty) {
			mm_free_page(pde & 0xFFFFF000);
			pd[i] = 0;
		}
	}
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
