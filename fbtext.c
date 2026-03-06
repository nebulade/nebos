/* Framebuffer text: draw 8x8 characters into VESA LFB. */

#include "fbtext.h"
#include "font_8x8.h"
#include "vga.h"

static int fbtext_use_lfb;
static unsigned int fbtext_cols, fbtext_rows;
static unsigned int fbtext_cur_col, fbtext_cur_row;
static int fbtext_cursor_visible;

/* Layout: header 5px down, rainbow 5px below header text, content 5px below rainbow. */
#define HEADER_TOP     5   /* row 0 text at y 5..14 (only ROW_HEIGHT scanlines drawn) */
#define RAINBOW_TOP   18   /* 5px below header text; line at y 18..19 */
#define CONTENT_TOP   25   /* 5px below rainbow; first content row at y 25..34 */

#define ROW_HEIGHT    10   /* drawn height per line (8x8 glyph + 2 line spacing); avoids clearing rainbow */

static inline unsigned int row_to_y0(unsigned int row)
{
	return row == 0 ? HEADER_TOP : (CONTENT_TOP + (row - 1) * ROW_HEIGHT);
}

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
	fbtext_use_lfb = 1;
	fbtext_cols = vga_fb_width() / 8;
	{
		unsigned int h = vga_fb_height();
		fbtext_rows = (h >= CONTENT_TOP + ROW_HEIGHT) ? (1 + (h - CONTENT_TOP) / ROW_HEIGHT) : 25;
	}
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
	vga_clear_rgb(0, 0, 0);
}

static void draw_glyph(unsigned int col, unsigned int row, unsigned char c, unsigned char attr)
{
	unsigned int x0 = col * 8;
	unsigned int y0 = row_to_y0(row);
	unsigned int r_fg, g_fg, b_fg, r_bg, g_bg, b_bg;
	unsigned int x, y;
	const unsigned char *glyph;

	attr_to_rgb(attr, 1, &r_fg, &g_fg, &b_fg);
	attr_to_rgb(attr, 0, &r_bg, &g_bg, &b_bg);

	glyph = &font_8x8[(unsigned int)c * FONT_8X8_ROWS];
	for (y = 0; y < ROW_HEIGHT; y++) {
		unsigned char row_bits = (y < FONT_8X8_ROWS) ? glyph[y] : 0;
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

	if (!fb)
		return;
	if (height <= CONTENT_TOP + ROW_HEIGHT)
		return;
	/* Scroll only the content area (below rainbow); header and rainbow stay fixed. */
	for (y = CONTENT_TOP; y < height - ROW_HEIGHT; y++) {
		unsigned int src_off = (y + ROW_HEIGHT) * pitch;
		unsigned int dst_off = y * pitch;
		for (i = 0; i < pitch; i++)
			fb[dst_off + i] = fb[src_off + i];
	}
	for (y = height - ROW_HEIGHT; y < height; y++) {
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

/* Slim vertical line cursor: 2px wide, matches row height. */
#define CURSOR_W  2
#define CURSOR_H  ROW_HEIGHT

void fbtext_show_cursor(void)
{
	unsigned int x0 = fbtext_cur_col * 8;
	unsigned int y0 = row_to_y0(fbtext_cur_row);
	unsigned int x, y;
	unsigned int r, g, b;

	fbtext_cursor_visible = 1;
	attr_to_rgb(0x0F, 1, &r, &g, &b); /* white line */
	for (y = 0; y < CURSOR_H; y++)
		for (x = 0; x < CURSOR_W; x++)
			vga_put_pixel_rgb(x0 + x, y0 + y, r, g, b);
}

/* Redraw only the cursor region (CURSOR_W x CURSOR_H) so the line is restored. */
static void draw_glyph_cursor_region(unsigned int col, unsigned int row, unsigned char c, unsigned char attr)
{
	unsigned int x0 = col * 8;
	unsigned int y0 = row_to_y0(row);
	unsigned int r_fg, g_fg, b_fg, r_bg, g_bg, b_bg;
	unsigned int x, y;
	const unsigned char *glyph;

	attr_to_rgb(attr, 1, &r_fg, &g_fg, &b_fg);
	attr_to_rgb(attr, 0, &r_bg, &g_bg, &b_bg);

	glyph = &font_8x8[(unsigned int)c * FONT_8X8_ROWS];
	for (y = 0; y < ROW_HEIGHT && y < CURSOR_H; y++) {
		unsigned char row_bits = (y < FONT_8X8_ROWS) ? glyph[y] : 0;
		for (x = 0; x < CURSOR_W; x++) {
			unsigned int px = x0 + x;
			unsigned int py = y0 + y;
			if (row_bits & (0x80u >> x))
				vga_put_pixel_rgb(px, py, r_fg, g_fg, b_fg);
			else
				vga_put_pixel_rgb(px, py, r_bg, g_bg, b_bg);
		}
	}
}

void fbtext_hide_cursor(unsigned char c, unsigned char attr)
{
	if (fbtext_cursor_visible) {
		fbtext_cursor_visible = 0;
		draw_glyph_cursor_region(fbtext_cur_col, fbtext_cur_row, c, attr);
	}
}

/* Rainbow gradient: hue 0..1535 (6*256) maps to R→Y→G→C→B→M→R. Integer interpolation. */
#define RAINBOW_SEG  256
#define RAINBOW_FULL (6 * RAINBOW_SEG)

static void hue_to_rgb(unsigned int hue, unsigned int *r, unsigned int *g, unsigned int *b)
{
	unsigned int seg = hue / RAINBOW_SEG;
	unsigned int f   = hue % RAINBOW_SEG;
	unsigned int r1, g1, b1, r2, g2, b2;
	if (seg >= 6)
		seg = 0;
	switch (seg) {
		case 0: r1=255; g1=0;   b1=0;   r2=255; g2=255; b2=0;   break; /* R → Y */
		case 1: r1=255; g1=255; b1=0;   r2=0;   g2=255; b2=0;   break; /* Y → G */
		case 2: r1=0;   g1=255; b1=0;   r2=0;   g2=255; b2=255; break; /* G → C */
		case 3: r1=0;   g1=255; b1=255; r2=0;   g2=0;   b2=255; break; /* C → B */
		case 4: r1=0;   g1=0;   b1=255; r2=255; g2=0;   b2=255; break; /* B → M */
		default: r1=255; g1=0; b1=255; r2=255; g2=0;   b2=0;   break; /* M → R */
	}
	*r = (r1 * (RAINBOW_SEG - f) + r2 * f) / RAINBOW_SEG;
	*g = (g1 * (RAINBOW_SEG - f) + g2 * f) / RAINBOW_SEG;
	*b = (b1 * (RAINBOW_SEG - f) + b2 * f) / RAINBOW_SEG;
}

#define RAINBOW_LINE_HEIGHT  2   /* 2-pixel tall line for visibility */

void fbtext_draw_rainbow_separator(void)
{
	unsigned int w = vga_fb_width();
	unsigned int x, y;
	unsigned int r, g, b;

	if (w == 0)
		return;
	for (y = 0; y < RAINBOW_LINE_HEIGHT; y++) {
		unsigned int py = RAINBOW_TOP + y;
		for (x = 0; x < w; x++) {
			unsigned int hue = (x * RAINBOW_FULL) / w;
			hue_to_rgb(hue, &r, &g, &b);
			vga_put_pixel_rgb(x, py, r, g, b);
		}
	}
}
