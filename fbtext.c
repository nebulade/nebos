/* Framebuffer text: draw 8x16 characters into VESA LFB. */

#include "fbtext.h"
#include "font_8x16.h"
#include "vga.h"

static int fbtext_use_lfb;
static unsigned int fbtext_cols, fbtext_rows;
static unsigned int fbtext_cur_col, fbtext_cur_row;
static int fbtext_cursor_visible;

/* VGA attribute (0-15) to RGB. Standard VGA palette. */
static void attr_to_rgb(unsigned char attr, int fg, unsigned int *r, unsigned int *g, unsigned int *b)
{
	static const unsigned int vga_rgb[16][3] = {
		{ 0x00, 0x00, 0x00 }, /* 0 black */
		{ 0x00, 0x00, 0xaa }, /* 1 blue */
		{ 0x00, 0xaa, 0x00 }, /* 2 green */
		{ 0x00, 0xaa, 0xaa }, /* 3 cyan */
		{ 0xaa, 0x00, 0x00 }, /* 4 red */
		{ 0xaa, 0x00, 0xaa }, /* 5 magenta */
		{ 0xaa, 0x55, 0x00 }, /* 6 brown */
		{ 0xaa, 0xaa, 0xaa }, /* 7 light gray */
		{ 0x55, 0x55, 0x55 }, /* 8 dark gray */
		{ 0x55, 0x55, 0xff }, /* 9 light blue */
		{ 0x55, 0xff, 0x55 }, /* 10 light green */
		{ 0x55, 0xff, 0xff }, /* 11 light cyan */
		{ 0xff, 0x55, 0x55 }, /* 12 light red */
		{ 0xff, 0x55, 0xff }, /* 13 light magenta */
		{ 0xff, 0xff, 0x55 }, /* 14 yellow */
		{ 0xff, 0xff, 0xff }, /* 15 white */
	};
	unsigned int idx = (unsigned int)(fg ? (attr & 0x0F) : ((attr >> 4) & 0x0F));
	*r = vga_rgb[idx][0];
	*g = vga_rgb[idx][1];
	*b = vga_rgb[idx][2];
}

void fbtext_init(void)
{
	if (!vga_has_vesa()) {
		fbtext_use_lfb = 0;
		fbtext_cols = 80;
		fbtext_rows = 25;
		return;
	}
	fbtext_use_lfb = 1;
	fbtext_cols = vga_fb_width() / 8;
	fbtext_rows = vga_fb_height() / 16;
	if (fbtext_cols == 0) fbtext_cols = 80;
	if (fbtext_rows == 0) fbtext_rows = 25;
	fbtext_cur_col = 0;
	fbtext_cur_row = 0;
	fbtext_cursor_visible = 0;
}

int fbtext_active(void)
{
	return fbtext_use_lfb;
}

unsigned int fbtext_get_cols(void)
{
	return fbtext_cols;
}

unsigned int fbtext_get_rows(void)
{
	return fbtext_rows;
}

void fbtext_clear(void)
{
	if (!fbtext_use_lfb)
		return;
	vga_clear_rgb(0, 0, 0);
}

static void draw_glyph(unsigned int col, unsigned int row, unsigned char c, unsigned char attr)
{
	unsigned int x0 = col * 8;
	unsigned int y0 = row * 16;
	unsigned int r_fg, g_fg, b_fg, r_bg, g_bg, b_bg;
	unsigned int x, y;
	const unsigned char *glyph;

	attr_to_rgb(attr, 1, &r_fg, &g_fg, &b_fg);
	attr_to_rgb(attr, 0, &r_bg, &g_bg, &b_bg);

	glyph = &font_8x16[(unsigned int)c * FONT_8X16_ROWS];
	for (y = 0; y < 16; y++) {
		unsigned char row_bits = glyph[y];
		for (x = 0; x < 8; x++) {
			unsigned int px = x0 + x;
			unsigned int py = y0 + y;
			if (row_bits & (0x80u >> x))
				vga_put_pixel_rgb(px, py, r_fg, g_fg, b_fg);
			else
				vga_put_pixel_rgb(px, py, r_bg, g_bg, b_bg);
		}
	}
}

void fbtext_put_char_at(unsigned int col, unsigned int row, unsigned char c, unsigned char attr)
{
	if (!fbtext_use_lfb)
		return;
	if (col >= fbtext_cols || row >= fbtext_rows)
		return;
	draw_glyph(col, row, c, attr);
}

void fbtext_scroll_up_one_line(void)
{
	unsigned int y, i;
	unsigned int pitch = vga_fb_pitch();
	volatile unsigned char *fb = (volatile unsigned char *)vga_fb_ptr();
	unsigned int height = vga_fb_height();

	if (!fbtext_use_lfb || !fb)
		return;
	if (height <= 16)
		return;
	for (y = 16; y < height - 16; y++) {
		unsigned int src_off = (y + 16) * pitch;
		unsigned int dst_off = y * pitch;
		for (i = 0; i < pitch; i++)
			fb[dst_off + i] = fb[src_off + i];
	}
	for (y = height - 16; y < height; y++) {
		unsigned int off = y * pitch;
		for (i = 0; i < pitch; i++)
			fb[off + i] = 0;
	}
}

void fbtext_set_cursor(unsigned int col, unsigned int row)
{
	if (col >= fbtext_cols || row >= fbtext_rows)
		return;
	fbtext_cur_col = col;
	fbtext_cur_row = row;
}

/* Cursor matches glyph height: font 'I' and caps use rows 0–7, rest is baseline padding. */
#define CURSOR_W  8
#define CURSOR_H  8

void fbtext_show_cursor(void)
{
	unsigned int x0 = fbtext_cur_col * 8;
	unsigned int y0 = fbtext_cur_row * 16;
	unsigned int x, y;
	unsigned int r, g, b;

	if (!fbtext_use_lfb)
		return;
	fbtext_cursor_visible = 1;
	attr_to_rgb(0x0F, 1, &r, &g, &b); /* white block */
	for (y = 0; y < CURSOR_H; y++)
		for (x = 0; x < CURSOR_W; x++)
			vga_put_pixel_rgb(x0 + x, y0 + y, r, g, b);
}

void fbtext_hide_cursor(unsigned char c, unsigned char attr)
{
	if (!fbtext_use_lfb)
		return;
	if (fbtext_cursor_visible) {
		fbtext_cursor_visible = 0;
		draw_glyph(fbtext_cur_col, fbtext_cur_row, c, attr);
	}
}
