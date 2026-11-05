/*
 * Intel Ironlake (Gen5) hardware cursor.
 *
 * The Ironlake display engine fetches the cursor through the global GTT, so
 * enabling it needs more than a register write: the 64x64 ARGB image must be
 * mapped at a free GTT slot.  BAR0 contains the MMIO registers followed by
 * the GTT at +2 MB; the BIOS has already mapped the VBE framebuffer at GTT
 * offset 0, and the table has free entries above it.
 *
 * Register semantics follow the Linux i915 Ironlake path:
 *  - CURACNTR/CURABASE/CURAPOS are per-pipe (A at 0x70080, B at 0x700c0).
 *  - 64x64 ARGB8888 cursor, MSB A:R:G:B, hotspot at (0,0).
 *  - CURCNTR bit 31 enables, mode 0x27 is 64x64 ARGB, bits 29:28 hold the
 *    pipe select.  CURBASE is written after CURPOS to arm the update.
 *  - The GTT PTE is the physical page, its bits 35:32 in PTE bits 7:4, the
 *    uncached mapping type (0) and the valid bit (1).
 */

#include "intel.h"
#include "cursor_shape.h"
#include "mm.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "vga.h"

#define INTEL_IRONLAKE_D 0x0042
#define INTEL_IRONLAKE_M 0x0046

#define INTEL_BAR0       0x10
#define INTEL_BAR2       0x18
#define INTEL_GTT_OFFSET 0x200000
#define INTEL_PGETBL_CTL 0x2020

/* Display pipe registers. */
#define DSPACNTR         0x70180
#define DSPASURF         0x7019c
#define DSPBCNTR         0x71180
#define DSPBSURF         0x7119c

/* Cursor registers (A; B is +0x40). */
#define CURACNTR         0x70080
#define CURABASE         0x70084
#define CURAPOS          0x70088
#define CURASURFLIVE     0x700ac
#define CUR_PIPE_B       0x40

#define DSP_PLANE_ENABLE       (1u << 31)
#define CURSOR_ENABLE          (1u << 31)
#define CURSOR_GAMMA_ENABLE    (1u << 26)
#define CURSOR_MODE_64_ARGB    0x27u
#define CURSOR_PIPE_SEL_SHIFT  28

#define CUR_SIZE   64
#define CUR_BYTES  (CUR_SIZE * CUR_SIZE * 4)
#define CUR_PAGES  (CUR_BYTES / 4096)

#define GTT_PTE_VALID    0x1u
#define GTT_PTE_UNCACHED 0x0u

static unsigned int intel_mmio;
static volatile unsigned int *intel_gtt;
static unsigned int intel_gtt_entries;
static unsigned int intel_gtt_limit;
static unsigned int intel_cursor_gtt;
static unsigned int intel_cursor_phys;
static unsigned int intel_pipe_mask;
static int intel_probe_ok;
static int intel_cursor_ok;

static inline unsigned int mmio_read(unsigned int ofs)
{
	return *(volatile unsigned int *)(intel_mmio + ofs);
}

static inline void mmio_write(unsigned int ofs, unsigned int val)
{
	*(volatile unsigned int *)(intel_mmio + ofs) = val;
}

static unsigned int intel_bar_base(unsigned int value)
{
	if (value & 1)
		return 0;
	return value & 0xFFFFFFF0u;
}

static unsigned int intel_bar_size(unsigned int bus, unsigned int dev,
				   unsigned int reg)
{
	unsigned int save, raw;

	save = pci_cfg_read32(bus, dev, 0, reg);
	pci_cfg_write32(bus, dev, 0, reg, 0xFFFFFFFFu);
	raw = pci_cfg_read32(bus, dev, 0, reg);
	pci_cfg_write32(bus, dev, 0, reg, save);
	if ((save & 1) || !(raw & 0xFFFFFFF0u))
		return 0;
	return (~(raw & 0xFFFFFFF0u)) + 1;
}

static unsigned int gtt_size_from_pgetbl(unsigned int pgetbl)
{
	switch (pgetbl & 0xE) {
	case 0:  return 512 * 1024;
	case 1:  return 256 * 1024;
	case 2:  return 128 * 1024;
	case 3:  return 1024 * 1024;
	case 4:  return 2048 * 1024;
	case 5:  return 1536 * 1024;
	default: return 512 * 1024;
	}
}

/* The BIOS leaves unused PTEs as all-ones rather than clearing the valid
 * bit, so test both forms. */
static int gtt_pte_free(unsigned int pte)
{
	return pte == 0xFFFFFFFFu || !(pte & GTT_PTE_VALID);
}

/* Find four consecutive invalid PTEs, preferably just above the visible
 * framebuffer and 64 KB aligned (the VT-d workaround alignment). */
static unsigned int gtt_find_slot(void)
{
	unsigned int i, j;
	unsigned int limit = intel_gtt_limit ? intel_gtt_limit
					    : intel_gtt_entries;
	unsigned int fb_pages;

	if (limit < 32)
		return 0;

	fb_pages = (vga_fb_pitch() * vga_fb_height() + 4095) / 4096;
	i = (fb_pages + 15) & ~15u;
	if (i < 16)
		i = 16;
	for (; i + CUR_PAGES <= limit; i += 16) {
		for (j = 0; j < CUR_PAGES; j++)
			if (!gtt_pte_free(intel_gtt[i + j]))
				break;
		if (j == CUR_PAGES)
			return i;
	}

	/* Fallback: high entries inside the aperture. */
	i = (limit - 64) & ~15u;
	while (i >= 16) {
		for (j = 0; j < CUR_PAGES; j++)
			if (!gtt_pte_free(intel_gtt[i + j]))
				break;
		if (j == CUR_PAGES)
			return i;
		i -= 16;
	}
	return 0;
}

/* The cursor fetch is happy with 4 KB on Ironlake unless VT-d is active;
 * still give it a 64 KB-aligned physical block so the VT-d workaround
 * alignment is covered as well. */
static int intel_alloc_cursor(void)
{
	unsigned int raw, aligned, pad, i;

	raw = mm_alloc_pages(32);
	if (!raw)
		return -1;
	aligned = (raw + 0xFFFFu) & ~0xFFFFu;
	pad = (aligned - raw) >> 12;
	for (i = 0; i < pad; i++)
		mm_free_page(raw + i * 4096);
	for (i = pad + CUR_PAGES; i < 32; i++)
		mm_free_page(raw + i * 4096);
	intel_cursor_phys = aligned;
	return 0;
}

static void intel_fill_cursor(unsigned int phys)
{
	unsigned int *img = (unsigned int *)phys;
	unsigned int i, r, c;

	for (i = 0; i < CUR_SIZE * CUR_SIZE; i++)
		img[i] = 0;

	for (r = 0; r < CURSOR_SHAPE_HEIGHT; r++) {
		unsigned short fill = cursor_shape_fill[r];
		unsigned short outline = cursor_shape_outline[r];

		for (c = 0; c < CURSOR_SHAPE_WIDTH; c++) {
			unsigned short bit = (unsigned short)(0x8000u >> c);
			unsigned int *px = &img[r * CUR_SIZE + c];

			if (fill & bit)
				*px = 0xFFFFFFFFu;      /* opaque white */
			else if (outline & bit)
				*px = 0xFF000000u;      /* opaque black */
		}
	}

	/* The GTT mapping is uncached, so push the WB image out to DRAM. */
	__asm__ volatile ("wbinvd" ::: "memory");
}

static int intel_map_cursor(unsigned int slot)
{
	unsigned int i;

	for (i = 0; i < CUR_PAGES; i++) {
		unsigned int pa = intel_cursor_phys + i * 4096;
		unsigned int pte = (pa & 0xFFFFF000u) |
				   ((pa >> 28) & 0xF0u) |
				   GTT_PTE_UNCACHED | GTT_PTE_VALID;

		intel_gtt[slot + i] = pte;
	}
	/* Posting read: also makes the display engine invalidate its TLB. */
	(void)intel_gtt[slot + CUR_PAGES - 1];

	/* Do not point the cursor plane at a slot the GTT did not accept. */
	for (i = 0; i < CUR_PAGES; i++) {
		unsigned int pa = intel_cursor_phys + i * 4096;

		if ((intel_gtt[slot + i] & 0xFFFFF000u) !=
		    (pa & 0xFFFFF000u))
			return -1;
	}
	return 0;
}

static unsigned int cursor_ctrl(unsigned int pipe)
{
	return CURSOR_ENABLE | CURSOR_GAMMA_ENABLE |
	       CURSOR_MODE_64_ARGB | (pipe << CURSOR_PIPE_SEL_SHIFT);
}

void intel_init(unsigned int bus, unsigned int dev, unsigned int device_id)
{
	unsigned int bar0, bar0_size, bar2_base, bar2_size;
	unsigned int lfb, lfb_gtt = 0xFFFFFFFFu;
	unsigned int pgetbl, gtt_size;
	unsigned int dspa, dspb, surfa, surfb;
	unsigned int slot;

	intel_probe_ok = 0;
	intel_cursor_ok = 0;
	intel_mmio = 0;
	intel_gtt = 0;
	intel_pipe_mask = 0;

	if (device_id != INTEL_IRONLAKE_D && device_id != INTEL_IRONLAKE_M)
		return;

	bar0 = pci_cfg_read32(bus, dev, 0, INTEL_BAR0);
	if ((bar0 & 1) || !(bar0 & 0xFFFFFFF0u))
		return;
	if ((bar0 & 0x6) == 0x4) {
		/* 64-bit BAR; the kernel cannot address anything above 4 GB. */
		if (pci_cfg_read32(bus, dev, 0, INTEL_BAR0 + 4))
			return;
	}
	bar0_size = intel_bar_size(bus, dev, INTEL_BAR0);
	if (bar0_size < 0x400000)
		bar0_size = 0x400000;
	if (bar0_size > 0x1000000)
		bar0_size = 0x1000000;

	intel_mmio = intel_bar_base(bar0);
	paging_map_mmio(intel_mmio, bar0_size);

	pgetbl = mmio_read(INTEL_PGETBL_CTL);
	if (pgetbl == 0xFFFFFFFFu) {
		serial_puts(SERIAL_COM1, "intel: no display MMIO\n");
		return;
	}

	/* A kexec leaves the previous kernel's cursor enabled, pointing at a
	 * GTT slot this kernel may no longer own.  Clear both cursor planes;
	 * the new cursor is enabled later from mouse_cursor_setup(). */
	mmio_write(CURACNTR, 0);
	mmio_write(CURACNTR + CUR_PIPE_B, 0);
	gtt_size = gtt_size_from_pgetbl(pgetbl);
	intel_gtt_entries = gtt_size / 4;
	intel_gtt = (volatile unsigned int *)(intel_mmio + INTEL_GTT_OFFSET);

	bar2_base = intel_bar_base(pci_cfg_read32(bus, dev, 0, INTEL_BAR2));
	bar2_size = intel_bar_size(bus, dev, INTEL_BAR2);
	intel_gtt_limit = intel_gtt_entries;
	if (bar2_size >= 0x1000000 && bar2_size / 4096 < intel_gtt_limit)
		intel_gtt_limit = bar2_size / 4096;
	lfb = vga_fb_lfb_addr();
	if (bar2_base && lfb >= bar2_base)
		lfb_gtt = (lfb - bar2_base) & 0xFFFFF000u;

	dspa = mmio_read(DSPACNTR);
	dspb = mmio_read(DSPBCNTR);
	surfa = mmio_read(DSPASURF) & 0xFFFFF000u;
	surfb = mmio_read(DSPBSURF) & 0xFFFFF000u;

	if ((dspa & DSP_PLANE_ENABLE) && surfa == lfb_gtt)
		intel_pipe_mask |= 1;
	if ((dspb & DSP_PLANE_ENABLE) && surfb == lfb_gtt)
		intel_pipe_mask |= 2;
	if (!intel_pipe_mask) {
		/* No surface match (or an unexpected BIOS layout): use every
		 * pipe the VBIOS left enabled. */
		if (dspa & DSP_PLANE_ENABLE)
			intel_pipe_mask |= 1;
		if (dspb & DSP_PLANE_ENABLE)
			intel_pipe_mask |= 2;
	}
	if (!intel_pipe_mask) {
		serial_puts(SERIAL_COM1, "intel: no enabled display pipe\n");
		return;
	}

	slot = gtt_find_slot();
	if (!slot) {
		serial_puts(SERIAL_COM1, "intel: no free GTT slot\n");
		return;
	}
	if (intel_alloc_cursor() != 0) {
		serial_puts(SERIAL_COM1, "intel: no cursor memory\n");
		return;
	}
	intel_cursor_gtt = slot * 4096;
	intel_fill_cursor(intel_cursor_phys);
	if (intel_map_cursor(slot) != 0) {
		serial_puts(SERIAL_COM1, "intel: GTT write did not stick\n");
		return;
	}

	serial_puts(SERIAL_COM1, "intel: Ironlake GTT ");
	serial_puthex(SERIAL_COM1, intel_gtt_entries * 4096);
	serial_puts(SERIAL_COM1, ", lfb gtt ");
	serial_puthex(SERIAL_COM1, lfb_gtt);
	serial_puts(SERIAL_COM1, ", pipe ");
	serial_puthex(SERIAL_COM1, intel_pipe_mask);
	serial_puts(SERIAL_COM1, ", cursor ");
	serial_puthex(SERIAL_COM1, intel_cursor_gtt);
	serial_puts(SERIAL_COM1, "\n");

	intel_probe_ok = 1;
}

int intel_available(void)
{
	return intel_probe_ok;
}

static void intel_cursor_program(unsigned int pipe, unsigned int x,
				 unsigned int y)
{
	unsigned int base = CURACNTR + (pipe ? CUR_PIPE_B : 0);
	unsigned int pos = (x & 0x7FFFu) | ((y & 0x7FFFu) << 16);

	mmio_write(base, cursor_ctrl(pipe));
	mmio_write(base + 8, pos);                  /* CURAPOS */
	mmio_write(base + 4, intel_cursor_gtt);     /* CURABASE */
	(void)mmio_read(base + 4);
}

int intel_cursor_setup(unsigned int x, unsigned int y)
{
	if (!intel_probe_ok || !intel_cursor_gtt)
		return -1;

	if (intel_pipe_mask & 1)
		intel_cursor_program(0, x, y);
	if (intel_pipe_mask & 2)
		intel_cursor_program(1, x, y);

	serial_puts(SERIAL_COM1, "intel: cursor enabled, live ");
	serial_puthex(SERIAL_COM1, mmio_read(CURASURFLIVE));
	serial_puts(SERIAL_COM1, "\n");
	intel_cursor_ok = 1;
	return 0;
}

void intel_cursor_move(unsigned int x, unsigned int y)
{
	unsigned int pos;

	if (!intel_cursor_ok)
		return;
	pos = (x & 0x7FFFu) | ((y & 0x7FFFu) << 16);
	if (intel_pipe_mask & 1) {
		mmio_write(CURAPOS, pos);
		mmio_write(CURABASE, intel_cursor_gtt);
	}
	if (intel_pipe_mask & 2) {
		mmio_write(CURAPOS + CUR_PIPE_B, pos);
		mmio_write(CURABASE + CUR_PIPE_B, intel_cursor_gtt);
	}
}
