/* Alpha-blended glyph blitters.  Separate from the rasterizer so ngui
 * (RGB565 canvas) and uwm_lib (16/24/32-bit shadow) share the same math. */
#include "ufont.h"

int ufont_blit_rgb565(unsigned short *px, int w, int h,
		      int x, int baseline, const struct ufont_glyph *g,
		      unsigned short color,
		      int cx, int cy, int cw, int ch)
{
	int row, col, touched = 0;
	int fr, fg, fb;

	if (!px || !g || !g->mask || w <= 0 || h <= 0)
		return 0;
	fr = (color >> 11) & 0x1F;
	fg = (color >> 5) & 0x3F;
	fb = color & 0x1F;
	for (row = 0; row < (int)g->h; row++) {
		int py = baseline + g->yoff + row;

		if (py < 0 || py >= h || py < cy || py >= cy + ch)
			continue;
		for (col = 0; col < (int)g->w; col++) {
			int px_x = x + g->xoff + col;
			unsigned int a;

			if (px_x < 0 || px_x >= w || px_x < cx ||
			    px_x >= cx + cw)
				continue;
			a = g->mask[row * (int)g->w + col];
			if (!a)
				continue;
			if (a >= 255) {
				px[py * w + px_x] = color;
			} else {
				unsigned int d = px[py * w + px_x];
				unsigned int dr = (d >> 11) & 0x1F;
				unsigned int dg = (d >> 5) & 0x3F;
				unsigned int db = d & 0x1F;
				unsigned int r = (fr * a + dr * (255 - a) +
						  127) / 255;
				unsigned int gg = (fg * a + dg * (255 - a) +
						   127) / 255;
				unsigned int b = (fb * a + db * (255 - a) +
						  127) / 255;

				px[py * w + px_x] =
					(unsigned short)((r << 11) |
							 (gg << 5) | b);
			}
			touched++;
		}
	}
	return touched;
}

int ufont_blit_rgb(void *pxv, int w, int h, int pitch, int bpp,
		   int x, int baseline, const struct ufont_glyph *g,
		   unsigned int rgb,
		   int cx, int cy, int cw, int ch)
{
	unsigned char *px = (unsigned char *)pxv;
	unsigned int fr = (rgb >> 16) & 0xFF;
	unsigned int fg = (rgb >> 8) & 0xFF;
	unsigned int fb = rgb & 0xFF;
	int row, col, touched = 0;

	if (!px || !g || !g->mask || w <= 0 || h <= 0)
		return 0;
	for (row = 0; row < (int)g->h; row++) {
		int py = baseline + g->yoff + row;
		unsigned char *line;

		if (py < 0 || py >= h || py < cy || py >= cy + ch)
			continue;
		line = px + (unsigned long)py * (unsigned int)pitch;
		for (col = 0; col < (int)g->w; col++) {
			int gx = x + g->xoff + col;
			unsigned int a;
			unsigned char *d;

			if (gx < 0 || gx >= w || gx < cx || gx >= cx + cw)
				continue;
			a = g->mask[row * (int)g->w + col];
			if (!a)
				continue;
			if (bpp == 16) {
				unsigned short *sp = (unsigned short *)line;
				unsigned short dst = sp[gx];
				unsigned int dr = (dst >> 11) & 0x1F;
				unsigned int dg = (dst >> 5) & 0x3F;
				unsigned int db = dst & 0x1F;
				unsigned int sr = (fr >> 3);
				unsigned int sg = (fg >> 2);
				unsigned int sb = (fb >> 3);
				unsigned int r = (sr * a + dr * (255 - a) +
						  127) / 255;
				unsigned int gg = (sg * a + dg * (255 - a) +
						   127) / 255;
				unsigned int b = (sb * a + db * (255 - a) +
						  127) / 255;

				sp[gx] = (unsigned short)((r << 11) |
							  (gg << 5) | b);
			} else {
				d = line + (unsigned int)gx * (unsigned int)(bpp / 8);
				if (a >= 255) {
					d[0] = (unsigned char)fb;
					d[1] = (unsigned char)fg;
					d[2] = (unsigned char)fr;
				} else {
					d[0] = (unsigned char)((fb * a +
						d[0] * (255 - a) + 127) / 255);
					d[1] = (unsigned char)((fg * a +
						d[1] * (255 - a) + 127) / 255);
					d[2] = (unsigned char)((fr * a +
						d[2] * (255 - a) + 127) / 255);
				}
				if (bpp == 32)
					d[3] = 0xFF;
			}
			touched++;
		}
	}
	return touched;
}
