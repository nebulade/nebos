/* Userland drawing primitives for the ring-3 WM.  Mirrors the kernel's
 * fbtext/gfx byte order so the mapped shadow matches the kernel console. */

#include "uwm_lib.h"
#include "font_8x16.h"

#define FONT_H 16

static const unsigned char pal_r[16] = {
	0, 0, 0, 0, 170, 170, 170, 170, 85, 85, 85, 85, 255, 255, 255, 255
};
static const unsigned char pal_g[16] = {
	0, 0, 0, 170, 0, 0, 85, 170, 85, 85, 255, 255, 85, 85, 255, 255
};
static const unsigned char pal_b[16] = {
	0, 0, 170, 0, 170, 170, 0, 170, 85, 255, 85, 255, 85, 255, 85, 255
};

void usurf_init(struct usurf *s, void *px, unsigned int w, unsigned int h,
		unsigned int pitch, unsigned int bpp)
{
	s->px = (unsigned char *)px;
	s->w = w;
	s->h = h;
	s->pitch = pitch;
	s->bpp = bpp;
}

static void put_px(struct usurf *s, unsigned int x, unsigned int y,
		   unsigned int r, unsigned int g, unsigned int b)
{
	unsigned char *p;

	if (x >= s->w || y >= s->h)
		return;
	p = s->px + (unsigned long)y * s->pitch;
	if (s->bpp == 16) {
		unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		unsigned short *d = (unsigned short *)p;
		d[x] = (unsigned short)v;
	} else {
		p += x * (s->bpp / 8);
		p[0] = (unsigned char)b;
		p[1] = (unsigned char)g;
		p[2] = (unsigned char)r;
		if (s->bpp == 32)
			p[3] = 0xFF;
	}
}

void usurf_fill(struct usurf *s, int x, int y, unsigned int w,
		unsigned int h, unsigned int rgb)
{
	unsigned int r = (rgb >> 16) & 0xFF;
	unsigned int g = (rgb >> 8) & 0xFF;
	unsigned int b = rgb & 0xFF;
	unsigned int i, j;

	if (x < 0) {
		w -= (unsigned int)(-x);
		x = 0;
	}
	if (y < 0) {
		h -= (unsigned int)(-y);
		y = 0;
	}
	if ((unsigned int)x >= s->w || (unsigned int)y >= s->h)
		return;
	if ((unsigned int)x + w > s->w)
		w = s->w - (unsigned int)x;
	if ((unsigned int)y + h > s->h)
		h = s->h - (unsigned int)y;

	if (s->bpp == 16) {
		unsigned short v = (unsigned short)(((r >> 3) << 11) |
					((g >> 2) << 5) | (b >> 3));
		for (j = 0; j < h; j++) {
			unsigned short *p = (unsigned short *)(s->px +
				(unsigned long)((unsigned int)y + j) * s->pitch) +
				(unsigned int)x;
			for (i = 0; i < w; i++)
				p[i] = v;
		}
	} else {
		unsigned int pb = s->bpp / 8;
		for (j = 0; j < h; j++) {
			unsigned char *p = s->px +
				(unsigned long)((unsigned int)y + j) * s->pitch +
				(unsigned int)x * pb;
			for (i = 0; i < w; i++) {
				p[0] = (unsigned char)b;
				p[1] = (unsigned char)g;
				p[2] = (unsigned char)r;
				if (pb == 4)
					p[3] = 0xFF;
				p += pb;
			}
		}
	}
}

void usurf_copy(struct usurf *s, int dx, int dy, int sx, int sy,
		unsigned int w, unsigned int h)
{
	unsigned int pb = s->bpp / 8;
	int backwards;

	if (w == 0 || h == 0 || dx < 0 || dy < 0 || sx < 0 || sy < 0)
		return;
	if ((unsigned int)dx + w > s->w || (unsigned int)sx + w > s->w ||
	    (unsigned int)dy + h > s->h || (unsigned int)sy + h > s->h)
		return;

	backwards = (dy > sy) || (dy == sy && dx > sx);
	{
		unsigned int i;
		for (i = 0; i < h; i++) {
			unsigned int syy = backwards ? (unsigned int)(sy + (int)h - 1 - (int)i)
						     : (unsigned int)sy + i;
			unsigned int dyy = backwards ? (unsigned int)(dy + (int)h - 1 - (int)i)
						     : (unsigned int)dy + i;
			unsigned char *src = s->px + (unsigned long)syy * s->pitch +
					     (unsigned int)sx * pb;
			unsigned char *dst = s->px + (unsigned long)dyy * s->pitch +
					     (unsigned int)dx * pb;
			unsigned int n = w * pb;

			if (backwards) {
				while (n--)
					dst[n] = src[n];
			} else {
				unsigned int k;
				for (k = 0; k < n; k++)
					dst[k] = src[k];
			}
		}
	}
}

void usurf_glyph(struct usurf *s, int x, int y, unsigned char c,
		 unsigned int fg, unsigned int bg, int transparent,
		 unsigned int height,
		 int cx, int cy, unsigned int cw, unsigned int ch)
{
	const unsigned char *glyph = &font_8x16[(unsigned int)c * FONT_8X16_ROWS];
	unsigned int fr = (fg >> 16) & 0xFF, fgn = (fg >> 8) & 0xFF, fb = fg & 0xFF;
	unsigned int br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
	unsigned int py, px;

	for (py = 0; py < height; py++) {
		unsigned char bits = (py < FONT_H) ? glyph[py] : 0;
		int yy = y + (int)py;

		if (yy < cy || yy >= cy + (int)ch)
			continue;
		for (px = 0; px < 8; px++) {
			int xx = x + (int)px;
			int on = (bits & (0x80u >> px)) != 0;

			if (xx < cx || xx >= cx + (int)cw)
				continue;
			if (on)
				put_px(s, (unsigned int)xx, (unsigned int)yy,
				       fr, fgn, fb);
			else if (!transparent)
				put_px(s, (unsigned int)xx, (unsigned int)yy,
				       br, bgn, bb);
		}
	}
}

void usurf_text(struct usurf *s, int x, int y, const char *str,
		unsigned int fg,
		int cx, int cy, unsigned int cw, unsigned int ch)
{
	unsigned int i = 0;

	while (str[i]) {
		usurf_glyph(s, x + (int)i * 8, y, (unsigned char)str[i],
			    fg, 0, 1, FONT_H, cx, cy, cw, ch);
		i++;
	}
}

void usurf_cells(struct usurf *s, const unsigned short *cells,
		 unsigned int cols, unsigned int rows,
		 int x0, int y0,
		 int cx, int cy, unsigned int cw, unsigned int ch)
{
	int col0 = (cx - x0) / 8;
	int col1 = (cx + (int)cw - x0 + 7) / 8;
	int row0 = (cy - y0) / 16;
	int row1 = (cy + (int)ch - y0 + 15) / 16;
	int row, col;

	if (col0 < 0) col0 = 0;
	if (row0 < 0) row0 = 0;
	if (col1 > (int)cols) col1 = (int)cols;
	if (row1 > (int)rows) row1 = (int)rows;
	if (col1 <= col0 || row1 <= row0)
		return;

	for (row = row0; row < row1; row++) {
		for (col = col0; col < col1; col++) {
			unsigned short cell = cells[(unsigned int)row * cols +
						    (unsigned int)col];
			unsigned char c = (unsigned char)(cell & 0xFF);
			unsigned char attr = (unsigned char)(cell >> 8);
			unsigned int fg = ((unsigned int)pal_r[attr & 15] << 16) |
				((unsigned int)pal_g[attr & 15] << 8) |
				pal_b[attr & 15];
			unsigned int bg = ((unsigned int)pal_r[(attr >> 4) & 15] << 16) |
				((unsigned int)pal_g[(attr >> 4) & 15] << 8) |
				pal_b[(attr >> 4) & 15];

			usurf_glyph(s, x0 + col * 8, y0 + row * 16, c,
				    fg, bg, 0, 16, cx, cy, cw, ch);
		}
	}
}

void usurf_canvas(struct usurf *s, const struct uwm_canvas *c,
		  int x0, int y0,
		  int cx, int cy, unsigned int cw, unsigned int ch)
{
	int x, y;
	int sx0 = 0, sy0 = 0;
	int sx1 = (int)c->w, sy1 = (int)c->h;

	if (cx > x0)
		sx0 = cx - x0;
	if (cy > y0)
		sy0 = cy - y0;
	if (cx + (int)cw < x0 + sx1)
		sx1 = cx + (int)cw - x0;
	if (cy + (int)ch < y0 + sy1)
		sy1 = cy + (int)ch - y0;
	if (sx1 <= sx0 || sy1 <= sy0)
		return;

	for (y = sy0; y < sy1; y++) {
		for (x = sx0; x < sx1; x++) {
			unsigned int p = c->px[(unsigned int)y * c->w +
					       (unsigned int)x];
			unsigned int r5 = (p >> 11) & 0x1F;
			unsigned int g6 = (p >> 5) & 0x3F;
			unsigned int b5 = p & 0x1F;
			unsigned int r = (r5 << 3) | (r5 >> 2);
			unsigned int g = (g6 << 2) | (g6 >> 4);
			unsigned int b = (b5 << 3) | (b5 >> 2);

			put_px(s, (unsigned int)(x0 + x),
			       (unsigned int)(y0 + y), r, g, b);
		}
	}
}
