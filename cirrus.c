/* Cirrus Logic GD5446 BitBLT acceleration.
   Reference: QEMU hw/display/cirrus_vga.c (register offsets, ROP codes). */

#include "cirrus.h"
#include "cursor_shape.h"
#include "pci.h"
#include "vga.h"

/* Sequencer (0x3C4/0x3C5). */
#define SR_UNLOCK     0x06
#define SR_UNLOCK_VAL 0x12
#define SR_CURSOR_X   0x10
#define SR_CURSOR_Y   0x11
#define SR_CURSOR_ATTR 0x12
#define SR_CURSOR_PAT 0x13

#define CURSOR_SHOW     0x01
#define CURSOR_HIDDENPEL 0x02

/* Graphics controller BitBLT registers (0x3CE/0x3CF). */
#define GR_WIDTH_LO    0x20
#define GR_WIDTH_HI    0x21
#define GR_HEIGHT_LO   0x22
#define GR_HEIGHT_HI   0x23
#define GR_DSTPITCH_LO 0x24
#define GR_DSTPITCH_HI 0x25
#define GR_SRCPITCH_LO 0x26
#define GR_SRCPITCH_HI 0x27
#define GR_DSTADDR_LO  0x28
#define GR_DSTADDR_MID 0x29
#define GR_DSTADDR_HI  0x2a
#define GR_SRCADDR_LO  0x2c
#define GR_SRCADDR_MID 0x2d
#define GR_SRCADDR_HI  0x2e
#define GR_MODE        0x30
#define GR_STATUS      0x31
#define GR_ROP         0x32
#define GR_MODEEXT     0x33

#define BLT_BUSY  0x01
#define BLT_START 0x02

#define BLTMODE_BACKWARDS      0x01
#define BLTMODE_PIXELWIDTH_MASK 0x30
#define BLTMODE_PIXELWIDTH16   0x10
#define BLTMODE_PIXELWIDTH24   0x20
#define BLTMODE_PIXELWIDTH32   0x30
#define BLTMODE_PATTERNCOPY    0x40
#define BLTMODE_COLOREXPAND    0x80

#define BLTMODEEXT_SOLIDFILL   0x04

#define ROP_SRC 0x0d

static int cirrus_present;

static void gr_write(unsigned char idx, unsigned char val)
{
	port_outb(0x3CE, idx);
	port_outb(0x3CF, val);
}

static unsigned char gr_read(unsigned char idx)
{
	port_outb(0x3CE, idx);
	return port_inb(0x3CF);
}

static void sr_write(unsigned char idx, unsigned char val)
{
	port_outb(0x3C4, idx);
	port_outb(0x3C5, val);
}

static unsigned char sr_read(unsigned char idx)
{
	port_outb(0x3C4, idx);
	return port_inb(0x3C5);
}

void cirrus_init(void)
{
	cirrus_present = 0;
	sr_write(SR_UNLOCK, SR_UNLOCK_VAL);
	if (sr_read(SR_UNLOCK) == SR_UNLOCK_VAL)
		cirrus_present = 1;
}

int cirrus_available(void)
{
	return cirrus_present;
}

void cirrus_blit(unsigned int sx, unsigned int sy,
		 unsigned int dx, unsigned int dy,
		 unsigned int w, unsigned int h)
{
	unsigned int bpp = vga_fb_bpp();
	unsigned int pb = (bpp == 16) ? 2 : (bpp == 24) ? 3 : 4;
	unsigned int pitch = vga_fb_pitch();
	unsigned int src = sy * pitch + sx * pb;
	unsigned int dst = dy * pitch + dx * pb;
	unsigned int width_bytes = w * pb;
	unsigned int mode = 0;

	if (bpp == 16)
		mode |= BLTMODE_PIXELWIDTH16;
	else if (bpp == 24)
		mode |= BLTMODE_PIXELWIDTH24;
	else
		mode |= BLTMODE_PIXELWIDTH32;

	if (dst > src) {
		/* Overlapping, moving down/right: walk from the bottom-right so the
		   source isn't clobbered. The engine runs backward with a positive
		   pitch written; the address points at the last byte of the region. */
		mode |= BLTMODE_BACKWARDS;
		src += (h - 1) * pitch + (width_bytes - 1);
		dst += (h - 1) * pitch + (width_bytes - 1);
	}

	gr_write(GR_WIDTH_LO, (unsigned char)((width_bytes - 1) & 0xFF));
	gr_write(GR_WIDTH_HI, (unsigned char)(((width_bytes - 1) >> 8) & 0xFF));
	gr_write(GR_HEIGHT_LO, (unsigned char)((h - 1) & 0xFF));
	gr_write(GR_HEIGHT_HI, (unsigned char)(((h - 1) >> 8) & 0xFF));
	gr_write(GR_DSTPITCH_LO, (unsigned char)(pitch & 0xFF));
	gr_write(GR_DSTPITCH_HI, (unsigned char)((pitch >> 8) & 0xFF));
	gr_write(GR_SRCPITCH_LO, (unsigned char)(pitch & 0xFF));
	gr_write(GR_SRCPITCH_HI, (unsigned char)((pitch >> 8) & 0xFF));
	gr_write(GR_DSTADDR_LO, (unsigned char)(dst & 0xFF));
	gr_write(GR_DSTADDR_MID, (unsigned char)((dst >> 8) & 0xFF));
	gr_write(GR_DSTADDR_HI, (unsigned char)((dst >> 16) & 0xFF));
	gr_write(GR_SRCADDR_LO, (unsigned char)(src & 0xFF));
	gr_write(GR_SRCADDR_MID, (unsigned char)((src >> 8) & 0xFF));
	gr_write(GR_SRCADDR_HI, (unsigned char)((src >> 16) & 0xFF));
	gr_write(GR_MODE, (unsigned char)mode);
	gr_write(GR_ROP, ROP_SRC);
	gr_write(GR_MODEEXT, 0);

	gr_write(GR_STATUS, 0);         /* ensure START is clear so the write triggers */
	gr_write(GR_STATUS, BLT_START);

	while (gr_read(GR_STATUS) & BLT_BUSY)
		;
}

void cirrus_fill(unsigned int x, unsigned int y,
		 unsigned int w, unsigned int h,
		 unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int bpp = vga_fb_bpp();
	unsigned int pb = (bpp == 16) ? 2 : (bpp == 24) ? 3 : 4;
	unsigned int pitch = vga_fb_pitch();
	unsigned int dst = y * pitch + x * pb;
	unsigned int width_bytes = w * pb;
	unsigned int mode = BLTMODE_PATTERNCOPY | BLTMODE_COLOREXPAND;

	/* Set the foreground color (the solid fill source). */
	if (bpp == 16) {
		unsigned short rgb565 = (unsigned short)
			(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		mode |= BLTMODE_PIXELWIDTH16;
		gr_write(0x01, (unsigned char)(rgb565 & 0xFF));
		gr_write(0x11, (unsigned char)((rgb565 >> 8) & 0xFF));
	} else if (bpp == 24) {
		mode |= BLTMODE_PIXELWIDTH24;
		gr_write(0x01, (unsigned char)b);
		gr_write(0x11, (unsigned char)g);
		gr_write(0x13, (unsigned char)r);
	} else {
		mode |= BLTMODE_PIXELWIDTH32;
		gr_write(0x01, (unsigned char)b);
		gr_write(0x11, (unsigned char)g);
		gr_write(0x13, (unsigned char)r);
		gr_write(0x15, 0);
	}

	gr_write(GR_WIDTH_LO, (unsigned char)((width_bytes - 1) & 0xFF));
	gr_write(GR_WIDTH_HI, (unsigned char)(((width_bytes - 1) >> 8) & 0xFF));
	gr_write(GR_HEIGHT_LO, (unsigned char)((h - 1) & 0xFF));
	gr_write(GR_HEIGHT_HI, (unsigned char)(((h - 1) >> 8) & 0xFF));
	gr_write(GR_DSTPITCH_LO, (unsigned char)(pitch & 0xFF));
	gr_write(GR_DSTPITCH_HI, (unsigned char)((pitch >> 8) & 0xFF));
	gr_write(GR_DSTADDR_LO, (unsigned char)(dst & 0xFF));
	gr_write(GR_DSTADDR_MID, (unsigned char)((dst >> 8) & 0xFF));
	gr_write(GR_DSTADDR_HI, (unsigned char)((dst >> 16) & 0xFF));
	gr_write(GR_MODE, (unsigned char)mode);
	gr_write(GR_ROP, ROP_SRC);
	gr_write(GR_MODEEXT, BLTMODEEXT_SOLIDFILL);

	gr_write(GR_STATUS, 0);         /* ensure START is clear so the write triggers */
	gr_write(GR_STATUS, BLT_START);

	while (gr_read(GR_STATUS) & BLT_BUSY)
		;
}

void cirrus_cursor_setup(void)
{
	unsigned char bitmap[256];
	unsigned int r, i;
	unsigned int vram = vga_fb_vram_kb() * 1024;
	unsigned char *dst;

	/* 32x32 two-plane cursor: AND plane (offset 0..127), XOR plane (128..255),
	   4 bytes per row.  The shared arrow is placed in the top-left of the
	   box (the cursor position is the top-left of the 32x32 box):
	     AND=0 XOR=0 -> transparent
	     AND=0 XOR=1 -> color0 (white fill)
	     AND=1 XOR=1 -> color1 (black outline)
	   The hidden DAC entries 0 and 15 are programmed white and black. */
	for (i = 0; i < 256; i++)
		bitmap[i] = 0;
	for (r = 0; r < CURSOR_SHAPE_HEIGHT; r++) {
		unsigned short fill = cursor_shape_fill[r];
		unsigned short outl = cursor_shape_outline[r];
		unsigned short xor = fill | outl;
		bitmap[r * 4 + 0] = (unsigned char)((outl >> 8) & 0xFF);        /* AND x=0..7  */
		bitmap[r * 4 + 1] = (unsigned char)(outl & 0xFF);               /* AND x=8..15 */
		bitmap[128 + r * 4 + 0] = (unsigned char)((xor >> 8) & 0xFF);   /* XOR x=0..7  */
		bitmap[128 + r * 4 + 1] = (unsigned char)(xor & 0xFF);          /* XOR x=8..15 */
	}

	/* Upload to the top of VRAM (SR[0x13] = 0 selects the first 256-byte block). */
	sr_write(SR_CURSOR_PAT, 0);
	dst = (unsigned char *)(vga_fb_lfb_addr() + vram - 16384);
	for (i = 0; i < 256; i++)
		dst[i] = bitmap[i];

	/* Cursor colors via the hidden DAC palette: entry 0 = white, 15 = black. */
	sr_write(SR_CURSOR_ATTR, CURSOR_HIDDENPEL);
	port_outb(0x3C8, 0);
	port_outb(0x3C9, 63); port_outb(0x3C9, 63); port_outb(0x3C9, 63);
	port_outb(0x3C8, 15);
	port_outb(0x3C9, 0); port_outb(0x3C9, 0); port_outb(0x3C9, 0);
	sr_write(SR_CURSOR_ATTR, CURSOR_SHOW);
}

void cirrus_cursor_move(unsigned int x, unsigned int y)
{
	/* Position is 11 bits; low 3 bits select the index alias, high 8 are data. */
	sr_write(SR_CURSOR_X | ((x & 0x7) << 5), (unsigned char)(x >> 3));
	sr_write(SR_CURSOR_Y | ((y & 0x7) << 5), (unsigned char)(y >> 3));
}

void cirrus_cursor_show(int show)
{
	unsigned char v = sr_read(SR_CURSOR_ATTR);
	if (show)
		v |= CURSOR_SHOW;
	else
		v &= ~CURSOR_SHOW;
	sr_write(SR_CURSOR_ATTR, v);
}
