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
#include "serial.h"
#include "vga.h"

/* Page-directory entry flags (4 MB page). */
#define PDE_PRESENT  0x001
#define PDE_WRITABLE 0x002
#define PDE_PWT      0x008
#define PDE_PCD      0x010
#define PDE_PS       0x080   /* 4 MB page */

/* How much of the LFB to map.  16 MB covers any mode up to 1280x1024x32
 * (~5 MB) with margin, and is independent of the BIOS's VRAM report. */
#define LFB_MAP_SIZE 0x1000000

static unsigned int page_dir[1024] __attribute__((aligned(4096)));

void paging_init(void)
{
	unsigned int i;
	const volatile struct vga_boot_fb_info *fb = VGA_BOOT_FB;
	/* Opaque asm hides the fixed 0x600 pointer from -Warray-bounds. */
	__asm__ volatile ("" : "+r"(fb));

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
		: "r"((unsigned int)&page_dir[0])
		: "eax", "memory");

	serial_puts(SERIAL_COM1, "paging: enabled (identity map)\n");
}
