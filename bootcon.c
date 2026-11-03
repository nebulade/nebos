/* Direct-to-LFB boot console (see bootcon.h).
 *
 * This deliberately does not use vga_fb_width()/vga_fb_*(): those call
 * vga_cache_boot_fb(), which probes the GPU and maps BARs, and this code runs
 * before mm_init/paging_init exist.  It reads the boot framebuffer info block
 * at 0x600 directly, exactly like paging.c does.
 */
#include "bootcon.h"
#include "bootinfo.h"
#include "font_8x8.h"
#include "mm.h"
#include "serial.h"
#include "vga.h"

#define SCALE  2
#define LINE_H (8 * SCALE + 2)
#define LEFT   8
#define TOP    8

static unsigned int fb_w, fb_h, fb_pitch, fb_bpp;
static unsigned int fb_base;
static volatile unsigned char *fb_lfb;
static int fb_swap;
static int fb_ok;
static int enabled;
static unsigned int cur_x, cur_y;
static unsigned int home_x, home_y;

/* Text log kept independently of the framebuffer: bootcon is disabled when
 * the WM starts, but the e1000e probe and later driver messages still go
 * through bootcon_puts().  The buffer is allocated from the frame allocator
 * (see bootcon_log_init) so it does not move the kernel's _end past the low
 * rootfs pack at 0x60000. */
static char *log_buf;
static unsigned int log_cap;
static unsigned int log_len;
static int log_truncated;

static void fb_px(unsigned int x, unsigned int y, unsigned int r,
		  unsigned int g, unsigned int b)
{
	volatile unsigned char *p;

	if (!fb_ok || x >= fb_w || y >= fb_h)
		return;
	p = fb_lfb + (unsigned long)y * fb_pitch +
	    (unsigned long)x * (fb_bpp / 8);
	if (fb_bpp == 16) {
		unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

		p[0] = (unsigned char)(v & 0xFF);
		p[1] = (unsigned char)(v >> 8);
	} else {
		p[0] = (unsigned char)(fb_swap ? r : b);
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)(fb_swap ? b : r);
		if (fb_bpp == 32)
			p[3] = 0;
	}
}

/* Clear the band the next line will be drawn into, so a reused line (after
 * wrapping at the bottom) cannot leave parts of an older message behind. */
static void clear_line(void)
{
	unsigned int x, y;

	for (y = 0; y < LINE_H; y++)
		for (x = 0; x < fb_w; x++)
			fb_px(x, cur_y + y, 0, 0, 0);
}

static void draw_char(unsigned int x, unsigned int y, unsigned char c)
{
	const unsigned char *g = &font_8x8[(unsigned int)c * 8];
	unsigned int row, col;

	for (row = 0; row < 8; row++) {
		unsigned char bits = g[row];

		for (col = 0; col < 8; col++) {
			if (bits & (0x80u >> col)) {
				unsigned int i, j;

				for (i = 0; i < SCALE; i++)
					for (j = 0; j < SCALE; j++)
						fb_px(x + col * SCALE + i,
						      y + row * SCALE + j,
						      255, 255, 255);
			}
		}
	}
}

void bootcon_init(void)
{
	const volatile struct vga_boot_fb_info *fb = VGA_BOOT_FB;

	/* Opaque asm hides the fixed 0x600 pointer from -Warray-bounds. */
	__asm__ volatile ("" : "+r"(fb));

	fb_ok = 0;
	enabled = 0;
	cur_x = LEFT;
	cur_y = TOP;
	if (!fb->valid || !fb->lfb_base)
		return;

	fb_w = fb->width;
	fb_h = fb->height;
	fb_pitch = fb->pitch;
	fb_bpp = fb->bpp;
	fb_base = fb->lfb_base;

	if (fb_bpp != 16 && fb_bpp != 24 && fb_bpp != 32)
		return;
	/* Some BIOSes report a pitch in pixels or too small; enforce a sane
	 * minimum exactly like vga_cache_boot_fb() does. */
	if (fb_pitch < fb_w * (fb_bpp / 8))
		fb_pitch = fb_w * (fb_bpp / 8);

	fb_lfb = (volatile unsigned char *)(unsigned long)fb->lfb_base;
	fb_swap = bootinfo_rgbx();
	if (!fb_w || !fb_h || !fb_pitch || !fb_lfb)
		return;

	fb_ok = 1;
	enabled = 1;
	/* Start away from the very top-left: some VBIOSes centre a mode that
	 * does not match the panel (e.g. 1280x1024 on a 1280x800 LVDS), which
	 * crops the outer rows/columns.  Starting at 1/8,1/4 keeps the log
	 * inside the visible area in both the centred and the scaled case. */
	home_x = fb_w / 8;
	home_y = fb_h / 4;
	if (home_x < 8)
		home_x = 8;
	if (home_y < 8)
		home_y = 8;
	cur_x = home_x;
	cur_y = home_y;
	clear_line();
}

int bootcon_ok(void)
{
	return fb_ok;
}

void bootcon_set_enabled(int on)
{
	enabled = on && fb_ok;
	if (enabled)
		clear_line();
}

static void newline(void)
{
	cur_x = home_x;
	cur_y += LINE_H;
	if (cur_y + 8 * SCALE > fb_h)
		cur_y = home_y;
	clear_line();
}

void bootcon_log_init(void)
{
	unsigned int mem;

	if (log_buf)
		return;
	mem = mm_alloc_pages(BOOTCON_LOG_PAGES);
	if (!mem)
		return;
	log_buf = (char *)mem;
	log_cap = BOOTCON_LOG_PAGES * PAGE_SIZE;
	log_len = 0;
	log_truncated = 0;
	log_buf[0] = '\0';
}

void bootcon_puts(const char *s)
{
	while (*s) {
		unsigned char c = (unsigned char)*s++;

		/* Keep the text even after bootcon_set_enabled(0) has hidden
		 * the framebuffer console. */
		if (log_buf) {
			if (log_len + 1 < log_cap) {
				log_buf[log_len++] = (char)c;
				log_buf[log_len] = '\0';
			} else {
				log_truncated = 1;
			}
		}

		if (!fb_ok || !enabled)
			continue;
		if (c == '\r') {
			cur_x = LEFT;
			continue;
		}
		if (c == '\n') {
			newline();
			continue;
		}
		if (cur_x + 8 * SCALE > fb_w)
			newline();
		draw_char(cur_x, cur_y, c);
		cur_x += 8 * SCALE;
	}
}

const char *bootcon_log(unsigned int *len)
{
	*len = log_len;
	return log_buf ? log_buf : "";
}

int bootcon_log_truncated(void)
{
	return log_truncated;
}

void bootcon_dec(unsigned int v)
{
	char buf[12];
	unsigned int i = 0;

	if (v == 0) {
		bootcon_puts("0");
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i) {
		char one[2];

		one[0] = buf[--i];
		one[1] = '\0';
		bootcon_puts(one);
	}
}

void bootcon_hex(unsigned int v)
{
	static const char hx[] = "0123456789ABCDEF";
	char buf[9];
	unsigned int i;

	for (i = 0; i < 8; i++)
		buf[i] = hx[(v >> (4 * (7 - i))) & 0xF];
	buf[8] = '\0';
	bootcon_puts(buf);
}

void bootcon_fb_info(void)
{
	bootcon_puts("fb ");
	bootcon_dec(fb_w);
	bootcon_puts("x");
	bootcon_dec(fb_h);
	bootcon_puts("x");
	bootcon_dec(fb_bpp);
	bootcon_puts(" pitch ");
	bootcon_dec(fb_pitch);
	bootcon_puts(" lfb 0x");
	bootcon_hex(fb_base);
	bootcon_puts("\n");

	/* Mirror to COM1 (and the netlog console) so a machine without a
	 * serial port can report the VBE mode before the WM repaints. */
	serial_puts(SERIAL_COM1, "fb ");
	serial_putdec(SERIAL_COM1, fb_w);
	serial_puts(SERIAL_COM1, "x");
	serial_putdec(SERIAL_COM1, fb_h);
	serial_puts(SERIAL_COM1, "x");
	serial_putdec(SERIAL_COM1, fb_bpp);
	serial_puts(SERIAL_COM1, " pitch ");
	serial_putdec(SERIAL_COM1, fb_pitch);
	serial_puts(SERIAL_COM1, " lfb ");
	serial_puthex(SERIAL_COM1, fb_base);
	serial_puts(SERIAL_COM1, "\n");
}
