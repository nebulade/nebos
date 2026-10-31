/* RGB565 window canvas: page-allocated, page-aligned (so it can be mapped
 * into a ring-3 process 1:1), plus bpp-aware blits into the shadow buffer. */

#include "gfx.h"
#include "mm.h"
#include "vga.h"

int gfx_alloc(struct gfx_canvas *c, unsigned int w, unsigned int h)
{
	unsigned int bytes, pages, i;

	c->px = 0;
	c->w = c->h = 0;
	c->seq = c->drawn = 0;
	if (w == 0 || h == 0 || w > 4096 || h > 4096)
		return -1;

	bytes = w * h * 2;
	pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	{
		unsigned int phys = mm_alloc_pages(pages);
		if (!phys)
			return -1;
		c->px = (unsigned short *)phys;
	}
	c->w = w;
	c->h = h;
	for (i = 0; i < w * h; i++)     /* fresh memory is not zeroed */
		c->px[i] = 0;
	c->seq = 1;                     /* first frame: draw it once */
	c->drawn = 0;
	return 0;
}

void gfx_free(struct gfx_canvas *c)
{
	unsigned int bytes, pages, i;

	if (!c->px)
		return;
	bytes = c->w * c->h * 2;
	pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	for (i = 0; i < pages; i++)
		mm_free_page((unsigned int)c->px + i * PAGE_SIZE);
	c->px = 0;
	c->w = c->h = 0;
	c->seq = c->drawn = 0;
}

/* One row of RGB565 -> the target's pixel format.  Byte order mirrors
 * fbtext.c's draw_glyph_at(); the 8-bit values replicate the high bits of the
 * 5/6-bit fields so black/white and the EGA palette stay exact.  Canvas
 * pixels are opaque, so the 32 bpp high byte is 0xFF. */
static void blit_row(unsigned char *dst, const unsigned short *src,
		     unsigned int n, unsigned int bpp)
{
	unsigned int x;

	if (bpp == 16) {
		unsigned short *d = (unsigned short *)dst;
		for (x = 0; x < n; x++)
			d[x] = src[x];
	} else if (bpp == 24) {
		for (x = 0; x < n; x++) {
			unsigned int p = src[x];
			unsigned int r5 = (p >> 11) & 0x1F;
			unsigned int g6 = (p >> 5) & 0x3F;
			unsigned int b5 = p & 0x1F;

			dst[0] = (unsigned char)((b5 << 3) | (b5 >> 2));
			dst[1] = (unsigned char)((g6 << 2) | (g6 >> 4));
			dst[2] = (unsigned char)((r5 << 3) | (r5 >> 2));
			dst += 3;
		}
	} else {
		unsigned int *d = (unsigned int *)dst;
		for (x = 0; x < n; x++) {
			unsigned int p = src[x];
			unsigned int r5 = (p >> 11) & 0x1F;
			unsigned int g6 = (p >> 5) & 0x3F;
			unsigned int b5 = p & 0x1F;

			d[x] = 0xFF000000u |
			       ((((r5 << 3) | (r5 >> 2))) << 16) |
			       ((((g6 << 2) | (g6 >> 4))) << 8) |
			       ((b5 << 3) | (b5 >> 2));
		}
	}
}

void gfx_blit(const struct gfx_canvas *c, unsigned int x0, unsigned int y0)
{
	struct surface *t = vga_target();
	unsigned int pb = (t->bpp == 16) ? 2 : (t->bpp == 24) ? 3 : 4;
	unsigned int y;

	if (!c->px || !t->px)
		return;
	if (y0 + c->h > t->h || x0 + c->w > t->w)
		return;
	for (y = 0; y < c->h; y++)
		blit_row(t->px + (unsigned long)(y0 + y) * t->pitch + (unsigned long)x0 * pb,
			 c->px + (unsigned long)y * c->w, c->w, t->bpp);
	vga_target_damage(x0, y0, c->w, c->h);
}

void gfx_blit_clip(const struct gfx_canvas *c, unsigned int x0, unsigned int y0,
		   unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch)
{
	struct surface *t = vga_target();
	unsigned int pb = (t->bpp == 16) ? 2 : (t->bpp == 24) ? 3 : 4;
	int dx0 = (int)x0, dy0 = (int)y0;
	int sx0 = 0, sy0 = 0, sx1 = (int)c->w, sy1 = (int)c->h;
	int y;

	if (!c->px || !t->px)
		return;

	/* Canvas coords of the clip rect (signed: the rect may miss entirely). */
	if ((int)cx > dx0)
		sx0 = (int)cx - dx0;
	if ((int)cy > dy0)
		sy0 = (int)cy - dy0;
	if ((int)(cx + cw) < dx0 + (int)c->w)
		sx1 = (int)(cx + cw) - dx0;
	if ((int)(cy + ch) < dy0 + (int)c->h)
		sy1 = (int)(cy + ch) - dy0;
	if (sx1 <= sx0 || sy1 <= sy0)
		return;

	for (y = sy0; y < sy1; y++)
		blit_row(t->px + (unsigned long)(y0 + (unsigned int)y) * t->pitch +
				(unsigned long)(x0 + (unsigned int)sx0) * pb,
			 c->px + (unsigned long)y * c->w + sx0,
			 (unsigned int)(sx1 - sx0), t->bpp);
	vga_target_damage(x0 + (unsigned int)sx0,
			  y0 + (unsigned int)sy0,
			  (unsigned int)(sx1 - sx0),
			  (unsigned int)(sy1 - sy0));
}
