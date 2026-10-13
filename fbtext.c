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

/* Draw one 8x8 glyph at pixel (x0, y0). height 8 = glyph only; 10 = glyph + 2 rows bg (terminal row).
   skip_bg: 1 = only draw foreground (transparent background for overlays).
   Writes directly into the shadow buffer (no per-pixel function call); the
   caller is responsible for marking the affected rows dirty. */
static void draw_glyph_at(unsigned int x0, unsigned int y0, unsigned char c,
		unsigned int r_fg, unsigned int g_fg, unsigned int b_fg,
		unsigned int r_bg, unsigned int g_bg, unsigned int b_bg,
		unsigned int height, int skip_bg)
{
	unsigned int x, y;
	const unsigned char *glyph = &font_8x8[(unsigned int)c * FONT_8X8_ROWS];
	unsigned char *fb = (unsigned char *)vga_fb_ptr();
	unsigned int pitch = vga_fb_pitch();
	unsigned int bpp = vga_fb_bpp();
	unsigned int fg32 = (r_fg << 16) | (g_fg << 8) | b_fg;
	unsigned int bg32 = (r_bg << 16) | (g_bg << 8) | b_bg;
	unsigned short fg16 = (unsigned short)(((r_fg >> 3) << 11) | ((g_fg >> 2) << 5) | (b_fg >> 3));
	unsigned short bg16 = (unsigned short)(((r_bg >> 3) << 11) | ((g_bg >> 2) << 5) | (b_bg >> 3));

	for (y = 0; y < height; y++) {
		unsigned char row_bits = (y < FONT_8X8_ROWS) ? glyph[y] : 0;
		unsigned int py = y0 + y;

		if (bpp == 16) {
			unsigned short *p = (unsigned short *)(fb + (unsigned long)py * pitch + x0 * 2);
			for (x = 0; x < 8; x++) {
				if (row_bits & (0x80u >> x))
					*p = fg16;
				else if (!skip_bg)
					*p = bg16;
				p++;
			}
		} else if (bpp == 24) {
			unsigned char *p = fb + (unsigned long)py * pitch + x0 * 3;
			for (x = 0; x < 8; x++) {
				if (row_bits & (0x80u >> x)) {
					p[0] = (unsigned char)b_fg; p[1] = (unsigned char)g_fg; p[2] = (unsigned char)r_fg;
				} else if (!skip_bg) {
					p[0] = (unsigned char)b_bg; p[1] = (unsigned char)g_bg; p[2] = (unsigned char)r_bg;
				}
				p += 3;
			}
		} else {
			unsigned int *p = (unsigned int *)(fb + (unsigned long)py * pitch + x0 * 4);
			for (x = 0; x < 8; x++) {
				if (row_bits & (0x80u >> x))
					*p = fg32;
				else if (!skip_bg)
					*p = bg32;
				p++;
			}
		}
	}
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
	unsigned int r_fg, g_fg, b_fg, r_bg, g_bg, b_bg;

	attr_to_rgb(attr, 1, &r_fg, &g_fg, &b_fg);
	attr_to_rgb(attr, 0, &r_bg, &g_bg, &b_bg);
	draw_glyph_at(col * 8, row_to_y0(row), c, r_fg, g_fg, b_fg, r_bg, g_bg, b_bg, ROW_HEIGHT, 0);
}

void fbtext_put_char_at(unsigned int col, unsigned int row, unsigned char c, unsigned char attr)
{
	if (col >= fbtext_cols || row >= fbtext_rows)
		return;
	draw_glyph(col, row, c, attr);
}

void fbtext_draw_cells(const unsigned short *cells, unsigned int cols, unsigned int rows,
		unsigned int x0, unsigned int y0)
{
	unsigned int row, col;
	for (row = 0; row < rows; row++) {
		for (col = 0; col < cols; col++) {
			unsigned short cell = cells[row * cols + col];
			unsigned char c = (unsigned char)(cell & 0xFF);
			unsigned char attr = (unsigned char)(cell >> 8);
			unsigned int rf, gf, bf, rb, gb, bb;
			attr_to_rgb(attr, 1, &rf, &gf, &bf);
			attr_to_rgb(attr, 0, &rb, &gb, &bb);
			draw_glyph_at(x0 + col * 8, y0 + row * ROW_HEIGHT, c,
				rf, gf, bf, rb, gb, bb, ROW_HEIGHT, 0);
		}
	}
	vga_fb_mark_dirty_rect_xy(y0, rows * ROW_HEIGHT, x0, x0 + cols * 8);
}

void fbtext_draw_cells_clip(const unsigned short *cells, unsigned int cols, unsigned int rows,
		unsigned int x0, unsigned int y0,
		unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch)
{
	int col0 = ((int)cx - (int)x0) / 8;
	int col1 = ((int)(cx + cw) - (int)x0 + 7) / 8;
	int row0 = ((int)cy - (int)y0) / ROW_HEIGHT;
	int row1 = ((int)(cy + ch) - (int)y0 + (ROW_HEIGHT - 1)) / ROW_HEIGHT;
	unsigned int row, col;

	if (col0 < 0) col0 = 0;
	if (row0 < 0) row0 = 0;
	if (col1 > (int)cols) col1 = (int)cols;
	if (row1 > (int)rows) row1 = (int)rows;
	if (col1 <= col0 || row1 <= row0)
		return;

	for (row = (unsigned int)row0; row < (unsigned int)row1; row++) {
		for (col = (unsigned int)col0; col < (unsigned int)col1; col++) {
			unsigned short cell = cells[row * cols + col];
			unsigned char c = (unsigned char)(cell & 0xFF);
			unsigned char attr = (unsigned char)(cell >> 8);
			unsigned int rf, gf, bf, rb, gb, bb;
			attr_to_rgb(attr, 1, &rf, &gf, &bf);
			attr_to_rgb(attr, 0, &rb, &gb, &bb);
			draw_glyph_at(x0 + col * 8, y0 + row * ROW_HEIGHT, c,
				rf, gf, bf, rb, gb, bb, ROW_HEIGHT, 0);
		}
	}
	vga_fb_mark_dirty_rect_xy(y0 + (unsigned int)row0 * ROW_HEIGHT,
		(unsigned int)(row1 - row0) * ROW_HEIGHT,
		x0 + (unsigned int)col0 * 8, x0 + (unsigned int)col1 * 8);
}

void fbtext_scroll_up_one_line(void)
{
	unsigned int y, i;
	unsigned int pitch = vga_fb_pitch();
	unsigned char *fb = (unsigned char *)vga_fb_ptr();
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
	/* Mark scrolled rows dirty so the flush copies them. */
	vga_fb_mark_dirty_rect(CONTENT_TOP, height - CONTENT_TOP);
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
	unsigned int r_fg, g_fg, b_fg, r_bg, g_bg, b_bg;
	unsigned int x, y;
	const unsigned char *glyph = &font_8x8[(unsigned int)c * FONT_8X8_ROWS];
	unsigned int x0 = col * 8;
	unsigned int y0 = row_to_y0(row);

	attr_to_rgb(attr, 1, &r_fg, &g_fg, &b_fg);
	attr_to_rgb(attr, 0, &r_bg, &g_bg, &b_bg);
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

/* Draw string at pixel (x0, y0) with fg RGB. transparent_bg 1 = only fg pixels (for overlays). */
void fbtext_draw_string_xy(unsigned int x0, unsigned int y0, const char *str,
		unsigned int r_fg, unsigned int g_fg, unsigned int b_fg, int transparent_bg)
{
	unsigned int i = 0;
	while (str[i]) {
		draw_glyph_at(x0 + i * 8, y0, (unsigned char)str[i], r_fg, g_fg, b_fg, 0, 0, 0, FONT_8X8_ROWS, transparent_bg);
		i++;
	}
	vga_fb_mark_dirty_rect_xy(y0, FONT_8X8_ROWS, x0, x0 + i * 8);
}

/* Like fbtext_draw_string_xy, but per-pixel clipped to (cx, cy, cw, ch): only
 * foreground pixels inside the clip rect are written. */
void fbtext_draw_string_xy_clip(unsigned int x0, unsigned int y0, const char *str,
		unsigned int r_fg, unsigned int g_fg, unsigned int b_fg,
		unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch)
{
	unsigned int i;

	for (i = 0; str[i]; i++) {
		unsigned int gx = x0 + i * 8;
		const unsigned char *glyph;
		unsigned int x, y;

		if (gx + 8 <= cx || gx >= cx + cw ||
		    y0 + FONT_8X8_ROWS <= cy || y0 >= cy + ch)
			continue;
		glyph = &font_8x8[(unsigned int)str[i] * FONT_8X8_ROWS];
		for (y = 0; y < FONT_8X8_ROWS; y++) {
			unsigned char bits = glyph[y];
			unsigned int py = y0 + y;
			unsigned int xa, xb;

			if (!bits || py < cy || py >= cy + ch)
				continue;
			xa = (gx > cx) ? gx : cx;
			xb = (gx + 8 < cx + cw) ? gx + 8 : cx + cw;
			for (x = xa; x < xb; x++)
				if (bits & (0x80u >> (x - gx)))
					vga_put_pixel_rgb(x, py, r_fg, g_fg, b_fg);
		}
	}
	vga_fb_mark_dirty_rect_xy(cy, ch, cx, cx + cw);
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
