/* ICO/CUR: pick the largest icon; decode its PNG or BMP DIB payload. */

#include "img_internal.h"

struct ico_ent {
	unsigned int w, h, bpp, size, off;
};

static int ico_choose(const unsigned char *d, unsigned int n,
		      struct ico_ent *best, unsigned int *type)
{
	unsigned int count, i;

	if (n < 6 || d[0] != 0 || d[1] != 0)
		return -1;
	*type = img_le16(d + 2);
	if ((*type != 1 && *type != 2) || (count = img_le16(d + 4)) == 0 ||
	    6u + count * 16u > n)
		return -1;
	best->w = best->h = 0;
	best->bpp = best->size = best->off = 0;
	for (i = 0; i < count; i++) {
		const unsigned char *e = d + 6 + i * 16;
		unsigned int w = e[0] ? e[0] : 256;
		unsigned int h = e[1] ? e[1] : 256;
		unsigned int bpp = img_le16(e + 6);
		unsigned int size = img_le32(e + 8);
		unsigned int off = img_le32(e + 12);

		if (size == 0 || off > n || size > n - off)
			continue;
		if (w * h > best->w * best->h ||
		    (w * h == best->w * best->h && bpp > best->bpp)) {
			best->w = w;
			best->h = h;
			best->bpp = bpp;
			best->size = size;
			best->off = off;
		}
	}
	return best->size ? 0 : -1;
}

static int ico_dib_info(const unsigned char *d, unsigned int n,
			unsigned int *w, unsigned int *h, unsigned int *bpp,
			unsigned int *dib, unsigned int *colors, int *topdown)
{
	unsigned int height;

	if (n < 40 || img_le32(d) < 40)
		return -1;
	*w = img_le32(d + 4);
	height = img_le32(d + 8);
	*topdown = ((int)height < 0);
	height = *topdown ? (unsigned int)(-(int)height) : height;
	*h = height / 2;
	*bpp = img_le16(d + 14);
	*dib = img_le32(d);
	*colors = img_le32(d + 32);
	if (*w == 0 || *h == 0 || *w > 256 || *h > 256)
		return -1;
	if (*bpp != 1 && *bpp != 4 && *bpp != 8 && *bpp != 24 && *bpp != 32)
		return -1;
	return 0;
}

int img_sniff_ico(const unsigned char *d, unsigned int n, struct img_info *out)
{
	struct ico_ent e;
	unsigned int type;
	const unsigned char *p;

	if (ico_choose(d, n, &e, &type))
		return IMG_FMT_UNKNOWN;
	p = d + e.off;
	out->format = IMG_FMT_ICO;
	out->has_alpha = 0;
	out->progressive = 0;
	if (e.size >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' &&
	    p[3] == 'G') {
		struct img_info sub;

		if (img_sniff_png(p, e.size, &sub) == IMG_FMT_UNKNOWN)
			return IMG_FMT_UNKNOWN;
		out->width = sub.width;
		out->height = sub.height;
		out->has_alpha = sub.has_alpha;
		return IMG_FMT_ICO;
	}
	{
		unsigned int w, h, bpp, dib, colors;
		int topdown;

		if (ico_dib_info(p, e.size, &w, &h, &bpp, &dib, &colors,
				 &topdown))
			return IMG_FMT_UNKNOWN;
		(void)topdown;
		out->width = w;
		out->height = h;
		out->has_alpha = 1;   /* the AND mask is always present */
	}
	return IMG_FMT_ICO;
}

int img_decode_ico(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	struct ico_ent e;
	unsigned int type, w, h, bpp, dib, colors, i;
	const unsigned char *p;

	if (ico_choose(d, n, &e, &type))
		return IMG_ERR_FORMAT;
	p = d + e.off;
	(void)type;
	if (e.size >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' &&
	    p[3] == 'G') {
		int r = img_decode_png(p, e.size, opt, m, out);

		if (r == 0)
			out->format = IMG_FMT_ICO;
		return r;
	}
	{
		int topdown;
		unsigned int pal_off, pal_n, row_stride, and_off, and_stride;
		unsigned int xor_off, x, y;
		unsigned char pal[256][3];
		unsigned int pal_a[256];

		if (ico_dib_info(p, e.size, &w, &h, &bpp, &dib, &colors,
				 &topdown))
			return IMG_ERR_FORMAT;
		if (!img_size_ok(w, h, opt))
			return IMG_ERR_TOOBIG;
		pal_n = 0;
		if (bpp <= 8) {
			pal_n = colors ? colors : (1u << bpp);
			if (pal_n > 256)
				pal_n = 256;
			pal_off = dib;
			if (pal_off + pal_n * 4u > e.size)
				return IMG_ERR_CORRUPT;
			for (i = 0; i < pal_n; i++) {
				const unsigned char *q = p + pal_off + i * 4;
				pal[i][0] = q[2];
				pal[i][1] = q[1];
				pal[i][2] = q[0];
				pal_a[i] = 255;
			}
		}
		row_stride = ((w * bpp + 31u) / 32u) * 4u;
		xor_off = dib + (bpp <= 8 ? pal_n * 4u : 0);
		if (xor_off > e.size || row_stride > e.size - xor_off ||
		    (unsigned long long)row_stride * h > e.size - xor_off)
			return IMG_ERR_CORRUPT;
		and_off = xor_off + row_stride * h;
		and_stride = ((w + 31u) / 32u) * 4u;

		{
			int r = img_alloc(m, w, h, IMG_FMT_ICO, 1, out);
			if (r)
				return r;
		}
		for (y = 0; y < h; y++) {
			const unsigned char *row = p + xor_off +
				(topdown ? y : (h - 1 - y)) * row_stride;
			for (x = 0; x < w; x++) {
				unsigned int rr = 0, gg = 0, bb = 0, aa = 255;
				unsigned int oi = y * w + x;

				if (bpp <= 8) {
					unsigned int idx;
					if (bpp == 8)
						idx = row[x];
					else if (bpp == 4)
						idx = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 15;
					else
						idx = (row[x / 8] >> (7 - (x & 7))) & 1;
					if (idx >= pal_n)
						idx = 0;
					rr = pal[idx][0];
					gg = pal[idx][1];
					bb = pal[idx][2];
					aa = pal_a[idx];
				} else if (bpp == 24) {
					const unsigned char *q = row + x * 3;
					bb = q[0];
					gg = q[1];
					rr = q[2];
				} else {
					const unsigned char *q = row + x * 4;
					bb = q[0];
					gg = q[1];
					rr = q[2];
					aa = q[3];
				}
				if (and_off + and_stride * (y + 1) <= e.size) {
					const unsigned char *arow = p + and_off +
						(topdown ? y : (h - 1 - y)) * and_stride;
					if (arow[x / 8] & (0x80u >> (x & 7)))
						aa = 0;
				}
				out->px[oi] = img_rgb565(rr, gg, bb);
				out->alpha[oi] = (unsigned char)aa;
			}
		}
	}
	return 0;
}
