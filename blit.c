/* Blit backend: software implementation (the fallback for every machine).
 *
 * The ops draw into the current render target, so a later layer compositor can
 * reuse them unchanged; copy() and present() work on the screen.  The software
 * paths are plain cached-memory loops (the target is the shadow buffer in RAM
 * for now; a flip/VRAM backend would replace them with GPU operations). */

#include "blit.h"
#include "cpu.h"
#include "gfx.h"
#include "vga.h"

static unsigned int pixel_bytes(unsigned int bpp)
{
	if (bpp == 16)
		return 2;
	if (bpp == 24)
		return 3;
	return 4;
}

/* Fill `dwords` with `value`; SSE2 for anything worth the call. */
static void fill_dwords(unsigned int *p, unsigned int value, unsigned int dwords)
{
	unsigned int i;

	if (dwords == 0)
		return;
	if (dwords >= 8 && cpu_has_sse2()) {
		vga_fb_fill_row_sse2(p, value, dwords);
		return;
	}
	for (i = 0; i < dwords; i++)
		p[i] = value;
}

/* Solid fill of the target. */
static void software_fill(unsigned int x, unsigned int y, unsigned int w,
			  unsigned int h, unsigned int rgb)
{
	struct surface *t = vga_target();
	unsigned int r = (rgb >> 16) & 0xFF;
	unsigned int g = (rgb >> 8) & 0xFF;
	unsigned int b = rgb & 0xFF;
	unsigned int row, x1, y1;

	if (!t || !t->px || w == 0 || h == 0)
		return;
	if (x >= t->w || y >= t->h)
		return;
	x1 = x + w;
	y1 = y + h;
	if (x1 > t->w)
		x1 = t->w;
	if (y1 > t->h)
		y1 = t->h;

	if (t->bpp == 16) {
		unsigned short val = (unsigned short)(((r >> 3) << 11) |
					((g >> 2) << 5) | (b >> 3));
		unsigned int val32 = (unsigned int)val | ((unsigned int)val << 16);
		for (row = y; row < y1; row++) {
			unsigned int *p = (unsigned int *)(t->px +
					(unsigned long)row * t->pitch + x * 2);
			unsigned int px = x1 - x;
			fill_dwords(p, val32, px / 2);
			if (px & 1)
				((unsigned short *)p)[px - 1] = val;
		}
	} else if (t->bpp == 24) {
		for (row = y; row < y1; row++) {
			unsigned char *p = t->px + (unsigned long)row * t->pitch + x * 3;
			unsigned int xx;
			for (xx = x; xx < x1; xx++) {
				p[0] = (unsigned char)b;
				p[1] = (unsigned char)g;
				p[2] = (unsigned char)r;
				p += 3;
			}
		}
	} else {
		unsigned int val = ((unsigned int)t->alpha << 24) |
				   (r << 16) | (g << 8) | b;
		unsigned int dwords = x1 - x;

		/* Long rows are bigger than the cache: normal stores would
		 * read every line in (RFO) before overwriting it.  Stream
		 * them instead and order once at the end. */
		if (dwords >= 32 && cpu_has_sse2()) {
			for (row = y; row < y1; row++)
				vga_fb_fill_row_nt((unsigned int *)(t->px +
					(unsigned long)row * t->pitch + x * 4),
					val, dwords);
			__asm__ volatile ("sfence" ::: "memory");
		} else {
			for (row = y; row < y1; row++) {
				unsigned int *p = (unsigned int *)(t->px +
						(unsigned long)row * t->pitch + x * 4);
				fill_dwords(p, val, dwords);
			}
		}
	}
	vga_target_damage(x, y, x1 - x, y1 - y);
}

/* Overlap-safe row copy (dword when aligned, bytes otherwise). */
static void row_move(unsigned char *dst, const unsigned char *src, unsigned int n)
{
	if (dst == src || n == 0)
		return;

	if ((((unsigned long)dst | (unsigned long)src) & 3) == 0) {
		unsigned int words = n >> 2;
		unsigned int i;
		unsigned int *dw = (unsigned int *)dst;
		const unsigned int *sw = (const unsigned int *)src;

		if (dst < src) {
			for (i = 0; i < words; i++)
				dw[i] = sw[i];
			for (i = words << 2; i < n; i++)
				dst[i] = src[i];
		} else {
			/* Overlapping move down: back to front. */
			for (i = n; i > (words << 2); i--)
				dst[i - 1] = src[i - 1];
			for (i = words; i-- > 0; )
				dw[i] = sw[i];
		}
		return;
	}

	if (dst < src) {
		unsigned int i;
		for (i = 0; i < n; i++)
			dst[i] = src[i];
	} else {
		while (n--)
			dst[n] = src[n];
	}
}

/* Rectangle copy inside the target (a window move). */
static void software_copy(unsigned int dx, unsigned int dy, unsigned int sx,
			  unsigned int sy, unsigned int w, unsigned int h)
{
	struct surface *t = vga_target();
	unsigned int pb, row_bytes, r;

	if (!t || !t->px || w == 0 || h == 0)
		return;
	pb = pixel_bytes(t->bpp);
	if (dx >= t->w || dy >= t->h || sx >= t->w || sy >= t->h)
		return;
	if (w > t->w - dx) w = t->w - dx;
	if (w > t->w - sx) w = t->w - sx;
	if (h > t->h - dy) h = t->h - dy;
	if (h > t->h - sy) h = t->h - sy;
	if (w == 0 || h == 0)
		return;
	row_bytes = w * pb;

	if (dy > sy || (dy == sy && dx > sx)) {
		for (r = h; r-- > 0; )
			row_move(t->px + (unsigned long)(dy + r) * t->pitch + dx * pb,
				 t->px + (unsigned long)(sy + r) * t->pitch + sx * pb,
				 row_bytes);
	} else {
		for (r = 0; r < h; r++)
			row_move(t->px + (unsigned long)(dy + r) * t->pitch + dx * pb,
				 t->px + (unsigned long)(sy + r) * t->pitch + sx * pb,
				 row_bytes);
	}
	vga_target_damage(dx, dy, w, h);
}

/* Blend an ARGB8888 source onto the target (uniform alpha, 32 bpp only).
 * The source alpha byte is the per-pixel coverage; `alpha` scales it. */
static void software_blend(unsigned int x, unsigned int y, unsigned int w,
			   unsigned int h, const struct surface *src,
			   unsigned int sx, unsigned int sy, unsigned int alpha)
{
	struct surface *t = vga_target();
	unsigned int row, col;

	if (!t || !t->px || !src || !src->px)
		return;
	if (t->bpp != 32 || src->bpp != 32 || w == 0 || h == 0)
		return;
	if (x >= t->w || y >= t->h || sx >= src->w || sy >= src->h)
		return;
	if (w > t->w - x) w = t->w - x;
	if (w > src->w - sx) w = src->w - sx;
	if (h > t->h - y) h = t->h - y;
	if (h > src->h - sy) h = src->h - sy;
	if (w == 0 || h == 0)
		return;

	for (row = 0; row < h; row++) {
		unsigned int *d = (unsigned int *)(t->px +
				(unsigned long)(y + row) * t->pitch + x * 4);
		const unsigned int *s = (const unsigned int *)(src->px +
				(unsigned long)(sy + row) * src->pitch + sx * 4);
		for (col = 0; col < w; col++) {
			unsigned int sv = s[col];
			unsigned int a = (sv >> 24) & 0xFF;
			unsigned int dv = d[col];
			unsigned int ia;

			if (alpha != 255)
				a = (a * alpha) / 255;
			if (a == 0)
				continue;
			if (a == 255) {
				d[col] = sv & 0x00FFFFFFu;
				continue;
			}
			ia = 255 - a;
			d[col] = ((((((sv >> 16) & 0xFF) * a + ((dv >> 16) & 0xFF) * ia) / 255) & 0xFF) << 16) |
				 ((((((sv >> 8) & 0xFF) * a + ((dv >> 8) & 0xFF) * ia) / 255) & 0xFF) << 8) |
				  ((((sv & 0xFF) * a + (dv & 0xFF) * ia) / 255) & 0xFF);
		}
	}
	vga_target_damage(x, y, w, h);
}

static void software_canvas(const struct gfx_canvas *c, unsigned int x,
			    unsigned int y, unsigned int cx, unsigned int cy,
			    unsigned int cw, unsigned int ch)
{
	gfx_blit_clip(c, x, y, cx, cy, cw, ch);
}

static void software_present(unsigned int max_bytes)
{
	vga_present_shadow(max_bytes);
}

static void software_flip(void)
{
	/* Single-buffered: present() already pushed the damage to VRAM. */
}

const struct blit_ops blit_software = {
	"software",
	software_fill,
	software_copy,
	software_blend,
	software_canvas,
	software_present,
	software_flip,
};

const struct blit_ops *blit = &blit_software;
