/*
 * ATI Radeon (R200-family / RV280) hardware cursor.
 *
 * The RV280's VBIOS sets a clean VBE mode, so (unlike Mach64/Rage) it needs
 * no CRTC fixup.  What it can give us is a hardware cursor: a 64x64 ARGB8888
 * overlay composited by the display engine, so moving the pointer no longer
 * costs a save/restore + dirty-row flush on the CPU.
 *
 * Register semantics follow the Linux "radeon" driver (R100/R200 legacy
 * cursor path):
 *
 *  - Cursor image: 64x64 ARGB8888 (32 bpp, 16 KB), byte order B,G,R,A.
 *  - RADEON_CUR_* are direct-MMIO registers in BAR2.
 *  - RADEON_CRTC_GEN_CNTL is an MM register file entry, accessed via
 *    RADEON_MM_INDEX / RADEON_MM_DATA.
 *  - Position format is (x << 16) | y (x in bits 30:16, y in bits 15:0).
 *  - Cursor mode is RADEON_CRTC_CUR_MODE_24BPP (2) in bits 22:20.
 *
 * NOTE: the bitmap is written straight into VRAM.  Do not use a local
 * [16 KB] array -- the kernel stack is only 2 KB (0x42800..0x42000) and a
 * 16 KB stack allocation overflows it.
 */

#include "radeon.h"
#include "pci.h"
#include "paging.h"
#include "serial.h"
#include "vga.h"

static unsigned int radeon_mmio;   /* BAR2 register aperture (1:1 mapped) */
static int radeon_present;

#define RADEON_MM_INDEX              0x0000
#define RADEON_MM_DATA               0x0004

#define RADEON_CRTC_GEN_CNTL         0x0050
#define  RADEON_CRTC_CUR_EN          (1u << 16)
#define  RADEON_CRTC_CUR_MODE_MASK   (7u << 20)
#define  RADEON_CRTC_CUR_MODE_24BPP  (2u << 20)

#define RADEON_CUR_OFFSET            0x0260
#define RADEON_CUR_HORZ_VERT_POSN    0x0264
#define RADEON_CUR_HORZ_VERT_OFF     0x0268

#define RADEON_CUR_SIZE              64          /* 64x64 pixels */
#define RADEON_CUR_BYTES             (RADEON_CUR_SIZE * RADEON_CUR_SIZE * 4)

/* Cursor bitmap byte offset within card memory (offscreen, above the visible
   scanout and inside the 16 MB mapped LFB window). */
#define RADEON_CUR_VRAM_OFS          0x800000

static inline unsigned int mmio_read(unsigned int ofs)
{
	volatile unsigned int *p =
		(volatile unsigned int *)(radeon_mmio + ofs);
	return *p;
}

static inline void mmio_write(unsigned int ofs, unsigned int v)
{
	volatile unsigned int *p =
		(volatile unsigned int *)(radeon_mmio + ofs);
	*p = v;
}

/* Access an MM register-file entry via MM_INDEX / MM_DATA. */
static inline unsigned int mmio_mm_read(unsigned int reg)
{
	mmio_write(RADEON_MM_INDEX, reg);
	return mmio_read(RADEON_MM_DATA);
}

static inline void mmio_mm_write(unsigned int reg, unsigned int v)
{
	mmio_write(RADEON_MM_INDEX, reg);
	mmio_write(RADEON_MM_DATA, v);
}

/* 16x16 arrow (MSB = column 0), matching the software cursor shape. */
static const unsigned short radeon_arrow[16] = {
	0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
	0xFC00, 0xE000, 0x8000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

void radeon_init(unsigned int bus, unsigned int dev, unsigned int device_id)
{
	unsigned int bar2, bar2_base, bar2_size;

	radeon_present = 0;
	radeon_mmio = 0;
	(void)device_id;

	/* BAR2 is the register aperture on Radeon. */
	bar2 = pci_cfg_read32(bus, dev, 0, 0x18);
	if ((bar2 & 1) || !(bar2 & 0xFFFFFFF0))
		return;
	bar2_base = bar2 & 0xFFFFFFF0;

	pci_cfg_write32(bus, dev, 0, 0x18, 0xFFFFFFFF);
	bar2_size = pci_cfg_read32(bus, dev, 0, 0x18);
	pci_cfg_write32(bus, dev, 0, 0x18, bar2);
	bar2_size = (~(bar2_size & 0xFFFFFFF0)) + 1;
	if (bar2_size < 0x10000)
		return;

	paging_map_mmio(bar2_base, bar2_size);

	radeon_mmio = bar2_base;

	/* Validate the aperture: a master abort reads back all-ones. */
	mmio_write(RADEON_MM_INDEX, RADEON_CRTC_GEN_CNTL);
	if (mmio_read(RADEON_MM_DATA) == 0xFFFFFFFF) {
		radeon_mmio = 0;
		return;
	}

	radeon_present = 1;
	serial_puts(SERIAL_COM1, "radeon: cursor MMIO ");
	serial_puthex(SERIAL_COM1, radeon_mmio);
	serial_puts(SERIAL_COM1, "\n");
}

int radeon_available(void)
{
	return radeon_present;
}

void radeon_cursor_setup(void)
{
	unsigned char *dst;
	unsigned int vram;
	unsigned int i, r, c, g;

	if (!radeon_present)
		return;

	vram = vga_fb_vram_kb() * 1024;
	if (vram < RADEON_CUR_VRAM_OFS + RADEON_CUR_BYTES) {
		radeon_present = 0;
		return;
	}

	dst = (unsigned char *)(vga_fb_lfb_addr() + RADEON_CUR_VRAM_OFS);

	/* Transparent background (alpha 0), then the opaque white arrow. */
	for (i = 0; i < RADEON_CUR_BYTES; i++)
		dst[i] = 0;
	for (r = 0; r < 16; r++) {
		unsigned short bits = radeon_arrow[r];
		for (c = 0; c < 16; c++) {
			if (bits & (0x8000u >> c)) {
				unsigned int off = (r * RADEON_CUR_SIZE + c) * 4;
				dst[off + 0] = 0xFF;  /* B */
				dst[off + 1] = 0xFF;  /* G */
				dst[off + 2] = 0xFF;  /* R */
				dst[off + 3] = 0xFF;  /* A */
			}
		}
	}

	mmio_write(RADEON_CUR_OFFSET, RADEON_CUR_VRAM_OFS);
	mmio_write(RADEON_CUR_HORZ_VERT_OFF, 0);    /* hotspot at (0,0) */
	mmio_write(RADEON_CUR_HORZ_VERT_POSN, 0);   /* position set on move */

	/* Enable the 24 bpp (ARGB8888) hardware cursor. */
	g = mmio_mm_read(RADEON_CRTC_GEN_CNTL);
	g &= ~(RADEON_CRTC_CUR_EN | RADEON_CRTC_CUR_MODE_MASK);
	g |= RADEON_CRTC_CUR_EN | RADEON_CRTC_CUR_MODE_24BPP;
	mmio_mm_write(RADEON_CRTC_GEN_CNTL, g);
}

void radeon_cursor_move(unsigned int x, unsigned int y)
{
	if (!radeon_present)
		return;
	mmio_write(RADEON_CUR_HORZ_VERT_POSN, (x << 16) | y);
}

void radeon_cursor_show(int show)
{
	unsigned int g;
	if (!radeon_present)
		return;
	g = mmio_mm_read(RADEON_CRTC_GEN_CNTL);
	if (show)
		g |= RADEON_CRTC_CUR_EN;
	else
		g &= ~RADEON_CRTC_CUR_EN;
	mmio_mm_write(RADEON_CRTC_GEN_CNTL, g);
}
