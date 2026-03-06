/* VGA mode 13h / mode 03h via direct register programming.
   Saves and restores the 8x16 font from plane 2 so text mode survives. */

#include "vga.h"

#define VGA_FB ((volatile unsigned char *)0xA0000)

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

/* ---- low-level register helpers ---- */

static void write_seq(unsigned char idx, unsigned char val)
{
	outb(0x3C4, idx);
	outb(0x3C5, val);
}

static void write_crtc(unsigned char idx, unsigned char val)
{
	outb(0x3D4, idx);
	outb(0x3D5, val);
}

static void write_gc(unsigned char idx, unsigned char val)
{
	outb(0x3CE, idx);
	outb(0x3CF, val);
}

static void crtc_unlock(void)
{
	outb(0x3D4, 0x11);
	outb(0x3D5, inb(0x3D5) & 0x7F);
}

/* ---- register tables ---- */

/* Mode 13h: 320x200, 256 colors, chain-4, framebuffer at 0xA0000 */
static const unsigned char misc_13  = 0x63;
static const unsigned char seq_13[]  = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const unsigned char crtc_13[] = {
	0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
	0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF
};
static const unsigned char gc_13[]   = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};
static const unsigned char ac_13[]   = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Mode 03h: 80x25, 16-color text, framebuffer at 0xB8000 */
static const unsigned char misc_03  = 0x67;
static const unsigned char seq_03[]  = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const unsigned char crtc_03[] = {
	0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
	0x00, 0x4F, 0x20, 0x0E, 0x00, 0x00, 0x00, 0x00,
	0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
};
static const unsigned char gc_03[]   = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};
static const unsigned char ac_03[]   = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
	0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x0C, 0x00, 0x0F, 0x08, 0x00
};

/* ---- font buffer: 256 chars x 32 bytes/char ---- */
#define FONT_SIZE 8192
static unsigned char font_buf[FONT_SIZE];

/* ---- set all VGA registers for a mode ---- */

static void set_regs(unsigned char misc,
	const unsigned char *seq, const unsigned char *crtc,
	const unsigned char *gc, const unsigned char *ac)
{
	unsigned int i;

	/* Misc Output */
	outb(0x3C2, misc);

	/* Sequencer: sync reset, write 1-4, release */
	write_seq(0, 0x01);
	for (i = 1; i < 5; i++)
		write_seq((unsigned char)i, seq[i]);
	write_seq(0, seq[0]);

	/* CRTC: unlock then write all 25 */
	crtc_unlock();
	for (i = 0; i < 25; i++)
		write_crtc((unsigned char)i, crtc[i]);

	/* Graphics Controller */
	for (i = 0; i < 9; i++)
		write_gc((unsigned char)i, gc[i]);

	/* Attribute Controller */
	(void)inb(0x3DA);  /* reset flip-flop */
	for (i = 0; i < 21; i++) {
		outb(0x3C0, (unsigned char)i);
		outb(0x3C0, ac[i]);
	}
	/* Re-enable display (PAS bit) */
	(void)inb(0x3DA);
	outb(0x3C0, 0x20);
}

/* ---- font save / restore via plane 2 ---- */

static void font_plane_access(void)
{
	write_seq(0, 0x01);       /* sync reset */
	write_seq(2, 0x04);       /* map mask: plane 2 only */
	write_seq(4, 0x06);       /* sequential access, extended memory */
	write_seq(0, 0x03);       /* release reset */
	write_gc(4, 0x02);        /* read map select: plane 2 */
	write_gc(5, 0x00);        /* mode: read/write mode 0 */
	write_gc(6, 0x00);        /* misc: A0000, 128K window */
}

static void text_plane_access(void)
{
	write_seq(0, 0x01);       /* sync reset */
	write_seq(2, 0x03);       /* map mask: planes 0 + 1 */
	write_seq(4, 0x02);       /* odd/even, extended memory */
	write_seq(0, 0x03);       /* release reset */
	write_gc(4, 0x00);        /* read map select: plane 0 */
	write_gc(5, 0x10);        /* odd/even mode */
	write_gc(6, 0x0E);        /* B8000, 32K window */
}

static void save_font(void)
{
	unsigned int i;

	font_plane_access();
	for (i = 0; i < FONT_SIZE; i++)
		font_buf[i] = VGA_FB[i];
}

static void restore_font(void)
{
	unsigned int i;

	font_plane_access();
	for (i = 0; i < FONT_SIZE; i++)
		VGA_FB[i] = font_buf[i];
	text_plane_access();
}

/* ---- public API ---- */

void vga_set_mode_13(void)
{
	save_font();
	set_regs(misc_13, seq_13, crtc_13, gc_13, ac_13);
}

void vga_set_mode_text(void)
{
	set_regs(misc_03, seq_03, crtc_03, gc_03, ac_03);
	restore_font();
}

void vga_put_pixel(int x, int y, unsigned char color)
{
	if (x >= 0 && x < VGA_GFX_WIDTH && y >= 0 && y < VGA_GFX_HEIGHT)
		VGA_FB[y * VGA_GFX_WIDTH + x] = color;
}

void vga_clear(unsigned char color)
{
	unsigned int i;
	unsigned int n = VGA_GFX_WIDTH * VGA_GFX_HEIGHT;

	for (i = 0; i < n; i++)
		VGA_FB[i] = color;
}
