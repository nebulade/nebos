/* On-screen kernel debug output: see kdbg.h. */

#include "kdbg.h"
#include "font_8x8.h"
#include "timer.h"
#include "vga.h"

volatile unsigned int kdbg_irq_count;
volatile unsigned int kdbg_irq_bytes;
volatile unsigned int kdbg_irq_other;
volatile unsigned int kdbg_last_sc;
volatile unsigned int kdbg_irq_masked;
volatile unsigned int kdbg_wm_count;
volatile unsigned int kdbg_wm_last;
volatile unsigned int kdbg_term_count;
volatile unsigned int kdbg_term_last;

static unsigned int fb_w, fb_h, fb_pitch, fb_bpp;
static volatile unsigned char *fb_lfb;
static int fb_ok;

#define KDBG_INFO_MAX 14
static char kdbg_info_buf[KDBG_INFO_MAX][40];
static unsigned char kdbg_info_used[KDBG_INFO_MAX];
static unsigned int kdbg_info_next;

void kdbg_init(void)
{
	fb_w = vga_fb_width();
	fb_h = vga_fb_height();
	fb_pitch = vga_fb_pitch();
	fb_bpp = vga_fb_bpp();
	fb_lfb = (volatile unsigned char *)(unsigned long)vga_fb_lfb_addr();
	fb_ok = (fb_w > 0 && fb_h > 60 && fb_pitch > 0 && fb_lfb &&
		 (fb_bpp == 16 || fb_bpp == 24 || fb_bpp == 32));
}

static void px(unsigned int x, unsigned int y,
	       unsigned int r, unsigned int g, unsigned int b)
{
	volatile unsigned char *p;

	if (!fb_ok || x >= fb_w || y >= fb_h)
		return;
	p = fb_lfb + (unsigned long)y * fb_pitch + (unsigned long)x * (fb_bpp / 8);
	if (fb_bpp == 16) {
		unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		p[0] = (unsigned char)(v & 0xFF);
		p[1] = (unsigned char)(v >> 8);
	} else if (fb_bpp == 24) {
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
	} else {
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
		p[3] = 0;
	}
}

static void rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		 unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			px(x + i, y + j, r, g, b);
}

static void draw_ch(unsigned int x, unsigned int y, char c, unsigned int sc,
		    unsigned int r, unsigned int g, unsigned int b)
{
	const unsigned char *gl = &font_8x8[(unsigned char)c * 8];
	unsigned int row, col, i, j;

	for (row = 0; row < 8; row++) {
		unsigned char bits = gl[row];
		for (col = 0; col < 8; col++) {
			if (bits & (0x80 >> col)) {
				for (i = 0; i < sc; i++)
					for (j = 0; j < sc; j++)
						px(x + col * sc + i,
						   y + row * sc + j, r, g, b);
			}
		}
	}
}

static void draw_str(unsigned int x, unsigned int y, unsigned int sc,
		     const char *s, unsigned int r, unsigned int g, unsigned int b)
{
	while (*s) {
		draw_ch(x, y, *s, sc, r, g, b);
		x += 8 * sc;
		s++;
	}
}

static unsigned int app_str(char *buf, unsigned int i, const char *s)
{
	while (*s)
		buf[i++] = *s++;
	return i;
}

static unsigned int app_dec(char *buf, unsigned int i, unsigned int v)
{
	char tmp[11];
	unsigned int n = 0, j;

	if (v == 0) {
		buf[i++] = '0';
		return i;
	}
	while (v) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	for (j = n; j > 0; j--)
		buf[i++] = tmp[j - 1];
	return i;
}

static unsigned int app_hex(char *buf, unsigned int i, unsigned int v,
			    unsigned int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	unsigned int d;

	for (d = 0; d < digits; d++)
		buf[i++] = hx[(v >> (4 * (digits - 1 - d))) & 0xF];
	return i;
}

static void kdbg_draw_info_row(unsigned int row)
{
	unsigned int y;

	if (!fb_ok || row >= KDBG_INFO_MAX || !kdbg_info_used[row])
		return;
	y = fb_h - 31 - row * 10;
	rect(0, y, 400, 9, 0, 0, 0);
	draw_str(1, y, 1, kdbg_info_buf[row], 255, 220, 80);
}

void kdbg_info(unsigned int row, const char *s)
{
	unsigned int i = 0;

	if (row >= KDBG_INFO_MAX)
		return;
	while (s[i] && i < sizeof(kdbg_info_buf[0]) - 1) {
		kdbg_info_buf[row][i] = s[i];
		i++;
	}
	kdbg_info_buf[row][i] = '\0';
	kdbg_info_used[row] = 1;
	kdbg_draw_info_row(row);
}

static void kdbg_redraw_info(void)
{
	unsigned int r;

	for (r = 0; r < KDBG_INFO_MAX; r++)
		kdbg_draw_info_row(r);
}

int kdbg_info_alloc(void)
{
	if (kdbg_info_next >= KDBG_INFO_MAX)
		return -1;
	return (int)kdbg_info_next++;
}

void kdbg_status(void)
{
	char buf[96];
	unsigned int i;

	if (!fb_ok)
		return;

	i = 0;
	i = app_str(buf, i, "IRQ ");
	i = app_dec(buf, i, kdbg_irq_count);
	i = app_str(buf, i, " SC ");
	i = app_hex(buf, i, kdbg_last_sc, 2);
	i = app_str(buf, i, " BY ");
	i = app_dec(buf, i, kdbg_irq_bytes);
	i = app_str(buf, i, " OT ");
	i = app_dec(buf, i, kdbg_irq_other);
	i = app_str(buf, i, " MK ");
	i = app_dec(buf, i, kdbg_irq_masked);
	buf[i] = '\0';
	rect(0, fb_h - 21, 400, 9, 0, 0, 0);
	draw_str(1, fb_h - 21, 1, buf, 120, 255, 120);

	i = 0;
	i = app_str(buf, i, "WM ");
	i = app_dec(buf, i, kdbg_wm_count);
	i = app_str(buf, i, " K ");
	i = app_hex(buf, i, kdbg_wm_last, 4);
	i = app_str(buf, i, " TRM ");
	i = app_dec(buf, i, kdbg_term_count);
	i = app_str(buf, i, " K ");
	i = app_hex(buf, i, kdbg_term_last, 4);
	buf[i] = '\0';
	rect(0, fb_h - 11, 400, 9, 0, 0, 0);
	draw_str(1, fb_h - 11, 1, buf, 120, 180, 255);

	kdbg_redraw_info();
}

/* Called from the PIT IRQ (~1 kHz). */
void kdbg_pit_tick(void)
{
	static unsigned int n;

	n++;
	if (!fb_ok)
		return;
	if ((n % 500) == 0) {
		int on = (int)((n / 500) & 1);
		/* 1 Hz heartbeat square, bottom-right corner. */
		rect(fb_w - 12, fb_h - 12, 10, 10,
		     on ? 0 : 60, on ? 255 : 60, on ? 0 : 60);
	}
	if ((n % 1000) == 0)
		kdbg_status();
}

void kdbg_panic(const char *name, unsigned int eip, unsigned int cr2)
{
	char buf[64];
	unsigned int i = 0;

	if (!fb_ok)
		return;

	rect(0, 0, fb_w, 52, 140, 0, 0);
	draw_str(4, 4, 2, "*** EXCEPTION ***", 255, 255, 255);
	draw_str(4, 26, 1, name, 255, 255, 0);
	i = app_str(buf, i, "EIP ");
	i = app_hex(buf, i, eip, 8);
	i = app_str(buf, i, "  CR2 ");
	i = app_hex(buf, i, cr2, 8);
	buf[i] = '\0';
	draw_str(4, 38, 1, buf, 255, 200, 200);
	kdbg_status();
}
