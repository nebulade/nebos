/* BMP: Windows/OS2 bitmaps.  1/4/8/16/24/32 bpp, BI_RGB, BI_BITFIELDS,
 * BI_RLE8/BI_RLE4, top-down and bottom-up rows. */

#include "img_internal.h"

static int bmp_info(const unsigned char *d, unsigned int n,
		    unsigned int *w, unsigned int *h, unsigned int *bpp,
		    unsigned int *comp, unsigned int *dib, int *topdown,
		    unsigned int *off, unsigned int *colors)
{
	unsigned int height;

	if (n < 26 || d[0] != 'B' || d[1] != 'M')
		return -1;
	*off = img_le32(d + 10);
	*dib = img_le32(d + 14);
	if (*dib < 12 || 14u + *dib > n)
		return -1;

	if (*dib == 12) {
		if (n < 26)
			return -1;
		*w = img_le16(d + 18);
		height = img_le16(d + 20);
		*bpp = img_le16(d + 24);
		*comp = 0;
		*colors = 0;
	} else {
		if (n < 34)
			return -1;
		*w = img_le32(d + 18);
		height = img_le32(d + 22);
		*bpp = img_le16(d + 28);
		*comp = img_le32(d + 30);
		*colors = img_le32(d + 46);
	}
	*topdown = ((int)height < 0);
	*h = *topdown ? (unsigned int)(-(int)height) : height;
	if (*w == 0 || *w > 32768 || *h == 0 || *h > 32768)
		return -1;
	if (*bpp != 1 && *bpp != 4 && *bpp != 8 && *bpp != 16 &&
	    *bpp != 24 && *bpp != 32)
		return -1;
	if (*comp != 0 && *comp != 1 && *comp != 2 && *comp != 3 &&
	    *comp != 6)
		return -1;
	return 0;
}

int img_sniff_bmp(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int w, h, bpp, comp, dib, off, colors;
	int topdown;

	if (bmp_info(d, n, &w, &h, &bpp, &comp, &dib, &topdown, &off,
		     &colors) != 0)
		return IMG_FMT_UNKNOWN;
	(void)topdown;
	out->format = IMG_FMT_BMP;
	out->width = w;
	out->height = h;
	out->has_alpha = 0;
	if (bpp == 32) {
		unsigned int amask = 0;
		if (dib >= 108 && n >= 14 + 108)
			amask = img_le32(d + 14 + 66);
		else if (comp == 6 && dib == 40) {
			if (n >= 14 + 40 + 16)
				amask = img_le32(d + 14 + 40 + 12);
		}
		out->has_alpha = amask != 0;
	}
	return IMG_FMT_BMP;
}

static unsigned int mask_shift(unsigned int mask)
{
	unsigned int s = 0;
	if (!mask)
		return 0;
	while (!(mask & 1)) {
		mask >>= 1;
		s++;
	}
	return s;
}

static unsigned int mask_max(unsigned int mask)
{
	unsigned int s = mask_shift(mask);
	return mask >> s;
}

static unsigned int mask_scale(unsigned int v, unsigned int mask)
{
	unsigned int max = mask_max(mask);
	unsigned int val;

	if (!mask || !max)
		return 0;
	val = (v & mask) >> mask_shift(mask);
	if (max == 255)
		return val;
	return (val * 255u + max / 2) / max;
}

static void put565(struct img_image *im, unsigned int x, unsigned int y,
		   unsigned int r, unsigned int g, unsigned int b,
		   unsigned int a)
{
	unsigned int i;

	if (x >= im->w || y >= im->h)
		return;
	i = y * im->w + x;
	im->px[i] = img_rgb565(r, g, b);
	if (im->alpha)
		im->alpha[i] = (unsigned char)a;
}

int img_decode_bmp(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	unsigned int w, h, bpp, comp, dib, off, colors;
	unsigned int stride, pal_off, pal_entries, pal_size = 4;
	unsigned int rmask = 0, gmask = 0, bmask = 0, amask = 0;
	unsigned int i, x, y;
	int topdown, alpha;
	unsigned char pal[256][3];
	unsigned int pal_a[256];
	int r;

	if (bmp_info(d, n, &w, &h, &bpp, &comp, &dib, &topdown, &off,
		     &colors) != 0)
		return IMG_ERR_FORMAT;
	if (!img_size_ok(w, h, opt))
		return IMG_ERR_TOOBIG;
	if (dib == 12)
		pal_size = 3;

	if (bpp == 16) {
		if ((comp == 3 || comp == 6)) {
			unsigned int mo = 14 + dib;
			if (dib == 40)
				mo = 14 + 40;
			else if (dib >= 108)
				mo = 14 + 40;   /* masks live in the DIB */
			if (mo + 12 > n)
				return IMG_ERR_CORRUPT;
			rmask = img_le32(d + mo);
			gmask = img_le32(d + mo + 4);
			bmask = img_le32(d + mo + 8);
			if (comp == 6 || (dib >= 52 && mo + 16 <= n))
				amask = img_le32(d + mo + 12);
		} else {
			rmask = 0x7C00;
			gmask = 0x03E0;
			bmask = 0x001F;
		}
	} else if (bpp == 32) {
		if (dib >= 108) {
			rmask = img_le32(d + 14 + 40);
			gmask = img_le32(d + 14 + 44);
			bmask = img_le32(d + 14 + 48);
			amask = img_le32(d + 14 + 56);
		} else if (comp == 3 || comp == 6) {
			unsigned int mo = 14 + dib;
			if (mo + 12 > n)
				return IMG_ERR_CORRUPT;
			rmask = img_le32(d + mo);
			gmask = img_le32(d + mo + 4);
			bmask = img_le32(d + mo + 8);
			if (comp == 6 && mo + 16 <= n)
				amask = img_le32(d + mo + 12);
		}
		if (!rmask && !gmask && !bmask) {
			rmask = 0x00FF0000;
			gmask = 0x0000FF00;
			bmask = 0x000000FF;
		}
	}
	alpha = (bpp == 32 && amask != 0);

	pal_off = 14 + dib;
	pal_entries = 0;
	if (bpp <= 8) {
		pal_entries = colors ? colors : (1u << bpp);
		if (pal_entries > 256)
			pal_entries = 256;
		if (pal_off + pal_entries * pal_size > n)
			return IMG_ERR_CORRUPT;
		for (i = 0; i < pal_entries; i++) {
			const unsigned char *p = d + pal_off + i * pal_size;
			pal[i][0] = p[2];
			pal[i][1] = p[1];
			pal[i][2] = p[0];
			pal_a[i] = 255;
		}
	}

	r = img_alloc(m, w, h, IMG_FMT_BMP, alpha, out);
	if (r)
		return r;

	if (comp == 1 || comp == 2) {
		unsigned int px = 0, py = 0;
		unsigned int pos = off;

		if (bpp != 8 && bpp != 4)
			return IMG_ERR_UNSUPPORTED;
		if (topdown)
			return IMG_ERR_UNSUPPORTED;
		while (pos + 1 < n) {
			unsigned int a = d[pos++], b = d[pos++];

			if (a == 0) {
				if (b == 0) {
					px = 0;
					py++;
					continue;
				}
				if (b == 1)
					break;
				if (b == 2) {
					if (pos + 2 > n)
						return IMG_ERR_CORRUPT;
					px += d[pos++];
					py += d[pos++];
					continue;
				}
				/* absolute mode */
				{
					unsigned int cnt = b;
					unsigned int k;

					if (bpp == 8) {
						if (pos + cnt > n)
							return IMG_ERR_CORRUPT;
						for (k = 0; k < cnt; k++) {
							unsigned int idx = d[pos++];
							if (idx < pal_entries)
								put565(out, px + k,
								       h - 1 - py,
								       pal[idx][0],
								       pal[idx][1],
								       pal[idx][2],
								       pal_a[idx]);
						}
						if ((cnt & 1) && pos < n)
							pos++;
					} else {
						unsigned int bytes = (cnt + 1) / 2;
						if (pos + bytes > n)
							return IMG_ERR_CORRUPT;
						for (k = 0; k < cnt; k++) {
							unsigned int idx = (k & 1) ?
								(d[pos + k / 2] & 15) :
								(d[pos + k / 2] >> 4);
							if (idx < pal_entries)
								put565(out, px + k,
								       h - 1 - py,
								       pal[idx][0],
								       pal[idx][1],
								       pal[idx][2],
								       pal_a[idx]);
						}
						pos += bytes;
						if (bytes & 1)
							pos++;
					}
					px += cnt;
				}
				continue;
			}
			/* encoded run */
			{
				unsigned int k;

				for (k = 0; k < a; k++) {
					unsigned int idx = b;

					if (bpp == 4)
						idx = (k & 1) ? (b & 15) : (b >> 4);
					if (idx < pal_entries)
						put565(out, px + k,
						       h - 1 - py,
						       pal[idx][0],
						       pal[idx][1],
						       pal[idx][2],
						       pal_a[idx]);
				}
				px += a;
			}
		}
		return 0;
	}

	stride = ((w * bpp + 31u) / 32u) * 4u;
	if (off > n || stride > n - off ||
	    (unsigned long long)stride * h > (unsigned long long)(n - off))
		return IMG_ERR_CORRUPT;

	for (y = 0; y < h; y++) {
		const unsigned char *row = d + off + (topdown ? y : (h - 1 - y)) * stride;

		for (x = 0; x < w; x++) {
			unsigned int rr, gg, bb, aa = 255;

			if (bpp <= 8) {
				unsigned int idx;

				if (bpp == 8)
					idx = row[x];
				else if (bpp == 4)
					idx = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 15;
				else
					idx = (row[x / 8] >> (7 - (x & 7))) & 1;
				if (idx >= pal_entries)
					idx = 0;
				rr = pal[idx][0];
				gg = pal[idx][1];
				bb = pal[idx][2];
				aa = pal_a[idx];
			} else if (bpp == 16) {
				unsigned int v = img_le16(row + x * 2);
				rr = mask_scale(v, rmask);
				gg = mask_scale(v, gmask);
				bb = mask_scale(v, bmask);
				if (amask)
					aa = mask_scale(v, amask);
			} else if (bpp == 24) {
				const unsigned char *p = row + x * 3;
				bb = p[0];
				gg = p[1];
				rr = p[2];
			} else {
				unsigned int v = img_le32(row + x * 4);
				if (rmask) {
					rr = mask_scale(v, rmask);
					gg = mask_scale(v, gmask);
					bb = mask_scale(v, bmask);
				} else {
					bb = v & 255;
					gg = (v >> 8) & 255;
					rr = (v >> 16) & 255;
				}
				if (alpha)
					aa = mask_scale(v, amask);
			}
			put565(out, x, y, rr, gg, bb, aa);
		}
	}
	return 0;
}
