/* Scaled, clipped RGB565 drawing with optional alpha compositing. */

#include "img.h"

unsigned int img_fit_scale(unsigned int iw, unsigned int ih,
			   unsigned int bw, unsigned int bh)
{
	unsigned int sx, sy;

	if (!iw || !ih || !bw || !bh)
		return 65536u;
	if (bw > 65535u) bw = 65535u;
	if (bh > 65535u) bh = 65535u;
	sx = (bw << 16) / iw;
	sy = (bh << 16) / ih;
	if (sx > sy)
		sx = sy;
	if (sx < 256)
		sx = 256;       /* 1/256 minimum */
	if (sx > (8u << 16))
		sx = 8u << 16;
	return sx;
}

static void get_px(const struct img_image *im, int x, int y,
		   unsigned int *r, unsigned int *g, unsigned int *b,
		   unsigned int *a)
{
	unsigned int v, i;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= (int)im->w) x = (int)im->w - 1;
	if (y >= (int)im->h) y = (int)im->h - 1;
	i = (unsigned int)y * im->w + (unsigned int)x;
	v = im->px[i];
	*r = ((v >> 11) & 31) * 255 / 31;
	*g = ((v >> 5) & 63) * 255 / 63;
	*b = (v & 31) * 255 / 31;
	*a = im->alpha ? im->alpha[i] : 255;
}

static unsigned short blend(unsigned int sr, unsigned int sg, unsigned int sb,
			    unsigned int sa, unsigned short dst)
{
	unsigned int dr, dg, db;

	if (sa == 255)
		return (unsigned short)(((sr >> 3) << 11) | ((sg >> 2) << 5) |
					(sb >> 3));
	if (sa == 0)
		return dst;
	dr = ((dst >> 11) & 31) * 255 / 31;
	dg = ((dst >> 5) & 63) * 255 / 63;
	db = (dst & 31) * 255 / 31;
	sr = (sr * sa + dr * (255 - sa) + 127) / 255;
	sg = (sg * sa + dg * (255 - sa) + 127) / 255;
	sb = (sb * sa + db * (255 - sa) + 127) / 255;
	return (unsigned short)(((sr >> 3) << 11) | ((sg >> 2) << 5) |
				(sb >> 3));
}

void img_draw(const struct img_image *im, const struct img_blit *b)
{
	unsigned int step, sw, sh;
	int lx, rx, ty, by;
	int x, y;

	if (!im || !b || !b->dst || !im->px || !im->w || !im->h ||
	    !b->dst_w || !b->dst_h || !b->scale || b->clip_w <= 0 ||
	    b->clip_h <= 0)
		return;

	sw = (unsigned int)(((unsigned long long)im->w * b->scale) >> 16);
	sh = (unsigned int)(((unsigned long long)im->h * b->scale) >> 16);
	if (!sw) sw = 1;
	if (!sh) sh = 1;
	/* ceil(2^32 / scale), without a 64-bit division (no libgcc in ring 3
	 * on the target).  The +1 only rounds a source coordinate up by less
	 * than one pixel. */
	step = 0xFFFFFFFFu / b->scale + 1u;

	lx = b->x;
	if (lx < b->clip_x) lx = b->clip_x;
	rx = b->x + (int)sw;
	if (rx > b->clip_x + b->clip_w) rx = b->clip_x + b->clip_w;
	if (rx > b->dst_w) rx = b->dst_w;
	ty = b->y;
	if (ty < b->clip_y) ty = b->clip_y;
	by = b->y + (int)sh;
	if (by > b->clip_y + b->clip_h) by = b->clip_y + b->clip_h;
	if (by > b->dst_h) by = b->dst_h;

	if (lx >= rx || ty >= by)
		return;

	/* 1:1, opaque, neat copy. */
	if (b->scale == 65536u && !im->alpha &&
	    !(b->flags & (IMG_BLIT_CHECKER | IMG_BLIT_OVER_DST))) {
		for (y = ty; y < by; y++) {
			int sy = y - b->y;
			const unsigned short *src = im->px + (unsigned int)sy * im->w;
			unsigned short *dst = b->dst + (unsigned int)y * b->dst_w;

			for (x = lx; x < rx; x++)
				dst[x] = src[x - b->x];
		}
		return;
	}

	for (y = ty; y < by; y++) {
		unsigned long long sy_fp =
			(unsigned long long)(y - b->y) * step;
		int sy0 = (int)(sy_fp >> 16);
		int sy1 = (int)((sy_fp + step) >> 16);

		if (sy1 <= sy0)
			sy1 = sy0 + 1;

		for (x = lx; x < rx; x++) {
			unsigned long long sx_fp =
				(unsigned long long)(x - b->x) * step;
			int sx0 = (int)(sx_fp >> 16);
			int sx1 = (int)((sx_fp + step) >> 16);
			unsigned int sr, sg, sb, sa;
			unsigned short d;

			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			if (!(b->flags & IMG_BLIT_FAST) && b->scale < 65536u) {
				unsigned int rsum = 0, gsum = 0, bsum = 0;
				unsigned int asum = 0, count = 0;
				int xx, yy;

				if (sx0 < 0) sx0 = 0;
				if (sy0 < 0) sy0 = 0;
				if (sx1 > (int)im->w) sx1 = (int)im->w;
				if (sy1 > (int)im->h) sy1 = (int)im->h;
				for (yy = sy0; yy < sy1; yy++) {
					for (xx = sx0; xx < sx1; xx++) {
						unsigned int r, g, bb, a;

						get_px(im, xx, yy, &r, &g, &bb, &a);
						rsum += r;
						gsum += g;
						bsum += bb;
						asum += a;
						count++;
					}
				}
				if (!count) {
					get_px(im, sx0, sy0, &sr, &sg, &sb, &sa);
				} else {
					sr = (rsum + count / 2) / count;
					sg = (gsum + count / 2) / count;
					sb = (bsum + count / 2) / count;
					sa = (asum + count / 2) / count;
				}
			} else if (!(b->flags & IMG_BLIT_FAST) &&
				   b->scale > 65536u) {
				int fx = (int)(sx_fp & 65535);
				int fy = (int)(sy_fp & 65535);
				unsigned int r0, g0, b0, a0, r1, g1, b1, a1;

				get_px(im, sx0, sy0, &r0, &g0, &b0, &a0);
				get_px(im, sx0 + 1, sy0, &r1, &g1, &b1, &a1);
				{
					unsigned int rr = (r0 * (65536 - fx) +
							   r1 * fx) >> 16;
					unsigned int gg = (g0 * (65536 - fx) +
							   g1 * fx) >> 16;
					unsigned int bb = (b0 * (65536 - fx) +
							   b1 * fx) >> 16;
					unsigned int aa = (a0 * (65536 - fx) +
							   a1 * fx) >> 16;

					get_px(im, sx0, sy0 + 1, &r0, &g0, &b0, &a0);
					get_px(im, sx0 + 1, sy0 + 1, &r1, &g1, &b1,
					       &a1);
					sr = (rr * (65536 - fy) +
					      ((r0 * (65536 - fx) + r1 * fx) >> 16) * fy) >> 16;
					sg = (gg * (65536 - fy) +
					      ((g0 * (65536 - fx) + g1 * fx) >> 16) * fy) >> 16;
					sb = (bb * (65536 - fy) +
					      ((b0 * (65536 - fx) + b1 * fx) >> 16) * fy) >> 16;
					sa = (aa * (65536 - fy) +
					      ((a0 * (65536 - fx) + a1 * fx) >> 16) * fy) >> 16;
				}
			} else {
				get_px(im, sx0, sy0, &sr, &sg, &sb, &sa);
			}

			{
				unsigned int di = (unsigned int)y * b->dst_w + x;
				unsigned short bg;

				if (b->flags & IMG_BLIT_OVER_DST)
					bg = b->dst[di];
				else if (b->flags & IMG_BLIT_CHECKER)
					bg = ((x >> 3) ^ (y >> 3)) & 1 ?
						(unsigned short)b->bg2 :
						(unsigned short)b->bg;
				else
					bg = (unsigned short)b->bg;
				d = (im->alpha && sa < 255) ?
					blend(sr, sg, sb, sa, bg) :
					(unsigned short)(((sr >> 3) << 11) |
							 ((sg >> 2) << 5) |
							 (sb >> 3));
				b->dst[di] = d;
			}
		}
	}
}
