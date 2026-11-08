/* 8x16 text-cell renderer for window content canvases (see text_canvas.h).
 *
 * The cell grid is the terminal's internal state (char + VGA attribute); this
 * file is the only place that knows the terminal cell geometry.  The window
 * server calls it with the damaged cell block and composites the resulting
 * RGB565 canvas.  No kernel dependencies, so the host test exercises the same
 * code. */

#include "text_canvas.h"
#include "font_8x16.h"

#define TEXT_CURSOR_W 2

/* Standard VGA palette expanded to RGB565; the same values as fbtext.c's
 * attr_to_rgb() and the userland WM palette. */
static const unsigned short text_pal565[16] = {
	0x0000, /*  0 black */
	0x0015, /*  1 blue */
	0x0540, /*  2 green */
	0x0555, /*  3 cyan */
	0xA800, /*  4 red */
	0xA815, /*  5 magenta */
	0xAAA0, /*  6 brown */
	0xAD55, /*  7 light gray */
	0x52AA, /*  8 dark gray */
	0x52BF, /*  9 light blue */
	0x57EA, /* 10 light green */
	0x57FF, /* 11 light cyan */
	0xFAAA, /* 12 light red */
	0xFABF, /* 13 light magenta */
	0xFFEA, /* 14 yellow */
	0xFFFF, /* 15 white */
};

static void put_cell(struct text_canvas *c, unsigned int col, unsigned int row,
		     unsigned char ch, unsigned char attr, int cursor)
{
	unsigned int x0 = col * TEXT_CELL_W;
	unsigned int y0 = row * TEXT_CELL_H;
	unsigned short fg = text_pal565[attr & 0x0F];
	unsigned short bg = text_pal565[(attr >> 4) & 0x0F];
	const unsigned char *glyph;
	unsigned int gy, gx;

	if (!c || !c->px || x0 >= c->w || y0 >= c->h)
		return;

	glyph = &font_8x16[(unsigned int)ch * FONT_8X16_ROWS];
	for (gy = 0; gy < TEXT_CELL_H; gy++) {
		unsigned int y = y0 + gy;
		unsigned short *dst;
		unsigned char bits;

		if (y >= c->h)
			break;
		dst = c->px + (unsigned long)y * c->w;
		bits = (gy < FONT_8X16_ROWS) ? glyph[gy] : 0;
		for (gx = 0; gx < TEXT_CELL_W; gx++) {
			unsigned int x = x0 + gx;

			if (x >= c->w)
				break;
			dst[x] = (bits & (0x80u >> gx)) ? fg : bg;
		}
	}

	if (!cursor)
		return;

	/* Block cursor: a full-cell-height bar in the first two columns. */
	for (gy = 0; gy < TEXT_CELL_H; gy++) {
		unsigned int y = y0 + gy;
		unsigned short *dst;
		unsigned int x;

		if (y >= c->h)
			break;
		dst = c->px + (unsigned long)y * c->w;
		for (x = 0; x < TEXT_CURSOR_W; x++) {
			if (x0 + x >= c->w)
				break;
			dst[x0 + x] = text_pal565[15];
		}
	}
}

void text_canvas_render(struct text_canvas *c,
			const unsigned short *cells,
			unsigned int cols, unsigned int rows,
			unsigned int col0, unsigned int row0,
			unsigned int ncols, unsigned int nrows,
			unsigned int cursor, int cursor_visible, int focused)
{
	unsigned int col, row;

	if (!c || !c->px || !cells || !cols || !rows)
		return;
	if (col0 >= cols || row0 >= rows)
		return;
	if (ncols > cols - col0)
		ncols = cols - col0;
	if (nrows > rows - row0)
		nrows = rows - row0;
	if (!ncols || !nrows)
		return;

	for (row = row0; row < row0 + nrows; row++) {
		for (col = col0; col < col0 + ncols; col++) {
			unsigned int idx = row * cols + col;
			unsigned short cell = cells[idx];
			unsigned char ch = (unsigned char)(cell & 0xFF);
			unsigned char attr = (unsigned char)(cell >> 8);
			int cur = (idx == cursor && cursor_visible && focused);

			put_cell(c, col, row, ch, attr, cur);
		}
	}
}
