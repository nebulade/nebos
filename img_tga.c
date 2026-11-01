/* TGA: uncompressed and RLE truecolor, grayscale and colormapped images. */

#include "img_internal.h"

struct tga_hdr {
	unsigned int idlen, cmap_type, type;
	unsigned int cmap_first, cmap_len, cmap_bits;
	unsigned int w, h, bpp, desc;
	unsigned int data_off;
	unsigned int pal_off;
};

static int tga_parse(const unsigned char *d, unsigned int n,
		     struct tga_hdr *t)
{
	unsigned int cmap_bytes;

	if (n < 18)
		return -1;
	t->idlen = d[0];
	t->cmap_type = d[1];
	t->type = d[2];
	t->cmap_first = img_le16(d + 3);
	t->cmap_len = img_le16(d + 5);
	t->cmap_bits = d[7];
	t->w = img_le16(d + 12);
	t->h = img_le16(d + 14);
	t->bpp = d[16];
	t->desc = d[17];

	if (t->type != 1 && t->type != 2 && t->type != 3 &&
	    t->type != 9 && t->type != 10 && t->type != 11)
		return -1;
	if (t->w == 0 || t->h == 0 || t->w > 32768 || t->h > 32768)
		return -1;
	if (t->cmap_type != 0 && t->cmap_type != 1)
		return -1;
	if (t->type == 1 || t->type == 9) {
		if (t->cmap_type != 1 || t->bpp != 8)
			return -1;
	} else if (t->type == 3 || t->type == 11) {
		if (t->bpp != 8 && t->bpp != 16)
			return -1;
	} else {
		if (t->bpp != 15 && t->bpp != 16 && t->bpp != 24 &&
		    t->bpp != 32)
			return -1;
	}
	if (t->cmap_type == 1) {
		if (t->cmap_bits != 15 && t->cmap_bits != 16 &&
		    t->cmap_bits != 24 && t->cmap_bits != 32)
			return -1;
		if (t->cmap_len == 0 || t->cmap_len > 256)
			return -1;
	}
	cmap_bytes = t->cmap_len * ((t->cmap_bits + 7) / 8);
	t->pal_off = 18 + t->idlen;
	t->data_off = t->pal_off + cmap_bytes;
	if (t->pal_off > n || t->data_off > n)
		return -1;
	if (t->type != 9 && t->type != 10 && t->type != 11) {
		unsigned long long bytes = (unsigned long long)t->w * t->h *
					   ((t->bpp + 7) / 8);
		if (bytes > (unsigned long long)(n - t->data_off))
			return -1;
	}
	return 0;
}

int img_sniff_tga(const unsigned char *d, unsigned int n, struct img_info *out)
{
	struct tga_hdr t;

	if (tga_parse(d, n, &t))
		return IMG_FMT_UNKNOWN;
	out->format = IMG_FMT_TGA;
	out->width = t.w;
	out->height = t.h;
	out->has_alpha = 0;
	if ((t.type == 2 || t.type == 10) && t.bpp == 32 && (t.desc & 15))
		out->has_alpha = 1;
	if ((t.type == 2 || t.type == 10) && t.bpp == 16 && (t.desc & 15))
		out->has_alpha = 1;
	if ((t.type == 3 || t.type == 11) && t.bpp == 16)
		out->has_alpha = 1;
	if ((t.type == 1 || t.type == 9) && t.cmap_bits == 32 && (t.desc & 15))
		out->has_alpha = 1;
	return IMG_FMT_TGA;
}

struct tga_px {
	unsigned int r, g, b, a;
};

static void tga_palette_entry(const unsigned char *p, unsigned int bits,
			      struct tga_px *px)
{
	px->a = 255;
	if (bits == 15 || bits == 16) {
		unsigned int v = img_le16(p);
		px->b = (v & 31) * 255 / 31;
		px->g = ((v >> 5) & 31) * 255 / 31;
		px->r = ((v >> 10) & 31) * 255 / 31;
		if (bits == 16)
			px->a = (v & 0x8000) ? 255 : 0;
	} else if (bits == 24) {
		px->b = p[0];
		px->g = p[1];
		px->r = p[2];
	} else {
		px->b = p[0];
		px->g = p[1];
		px->r = p[2];
		px->a = p[3];
	}
}

int img_decode_tga(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	struct tga_hdr t;
	struct tga_px pal[256];
	unsigned int i, x = 0, y = 0, pos;
	int rle, alpha, rcode;

	if (tga_parse(d, n, &t))
		return IMG_ERR_FORMAT;
	if (!img_size_ok(t.w, t.h, opt))
		return IMG_ERR_TOOBIG;
	rle = (t.type == 9 || t.type == 10 || t.type == 11);
	alpha = 0;
	if ((t.type == 2 || t.type == 10) && t.bpp == 32 && (t.desc & 15))
		alpha = 1;
	if ((t.type == 2 || t.type == 10) && t.bpp == 16 && (t.desc & 15))
		alpha = 1;
	if ((t.type == 3 || t.type == 11) && t.bpp == 16)
		alpha = 1;
	if ((t.type == 1 || t.type == 9) && t.cmap_bits == 32 && (t.desc & 15))
		alpha = 1;

	for (i = 0; i < 256; i++) {
		pal[i].r = pal[i].g = pal[i].b = 0;
		pal[i].a = 255;
	}
	if (t.cmap_type == 1) {
		unsigned int es = (t.cmap_bits + 7) / 8;

		for (i = 0; i < t.cmap_len && i < 256; i++)
			tga_palette_entry(d + t.pal_off + i * es, t.cmap_bits,
					  &pal[i]);
	}

	rcode = img_alloc(m, t.w, t.h, IMG_FMT_TGA, alpha, out);
	if (rcode)
		return rcode;

	pos = t.data_off;
	while (y < t.h && x < t.w) {
		unsigned int count = 1, rep = 0;

		if (rle) {
			unsigned int hdr;

			if (pos >= n)
				return IMG_ERR_CORRUPT;
			hdr = d[pos++];
			count = (hdr & 0x7F) + 1;
			rep = (hdr & 0x80) != 0;
		}
		if (rep) {
			struct tga_px px;
			unsigned int k;

			/* read one pixel */
			if (t.type == 1 || t.type == 9) {
				unsigned int idx;

				if (pos >= n)
					return IMG_ERR_CORRUPT;
				idx = d[pos++];
				idx = idx >= t.cmap_first ? idx - t.cmap_first : 0;
				px = (idx >= 256) ? pal[0] : pal[idx];
			} else if (t.type == 3 || t.type == 11) {
				if (pos + (t.bpp == 16 ? 2 : 1) > n)
					return IMG_ERR_CORRUPT;
				px.r = px.g = px.b = d[pos++];
				px.a = 255;
				if (t.bpp == 16) {
					px.a = d[pos++];
					if (!alpha)
						px.a = 255;
				}
			} else if (t.bpp == 15 || t.bpp == 16) {
				unsigned int v;

				if (pos + 2 > n)
					return IMG_ERR_CORRUPT;
				v = img_le16(d + pos);
				pos += 2;
				px.b = (v & 31) * 255 / 31;
				px.g = ((v >> 5) & 31) * 255 / 31;
				px.r = ((v >> 10) & 31) * 255 / 31;
				px.a = (t.bpp == 16 && alpha && !(v & 0x8000)) ? 0 : 255;
			} else if (t.bpp == 24) {
				if (pos + 3 > n)
					return IMG_ERR_CORRUPT;
				px.b = d[pos++];
				px.g = d[pos++];
				px.r = d[pos++];
				px.a = 255;
			} else {
				if (pos + 4 > n)
					return IMG_ERR_CORRUPT;
				px.b = d[pos++];
				px.g = d[pos++];
				px.r = d[pos++];
				px.a = alpha ? d[pos] : 255;
				pos++;
			}
			for (k = 0; k < count && y < t.h; k++) {
				unsigned int ox = (t.desc & 0x10) ? t.w - 1 - x : x;
				unsigned int oy = (t.desc & 0x20) ? y : t.h - 1 - y;
				unsigned int oi = oy * t.w + ox;

				out->px[oi] = img_rgb565(px.r, px.g, px.b);
				if (out->alpha)
					out->alpha[oi] = (unsigned char)px.a;
				if (++x >= t.w) {
					x = 0;
					y++;
				}
			}
		} else {
			unsigned int k;

			for (k = 0; k < count && y < t.h; k++) {
				struct tga_px px;
				unsigned int ox = (t.desc & 0x10) ? t.w - 1 - x : x;
				unsigned int oy = (t.desc & 0x20) ? y : t.h - 1 - y;
				unsigned int oi = oy * t.w + ox;

				if (t.type == 1 || t.type == 9) {
					unsigned int idx;

					if (pos >= n)
						return IMG_ERR_CORRUPT;
					idx = d[pos++];
					idx = idx >= t.cmap_first ? idx - t.cmap_first : 0;
					px = (idx >= 256) ? pal[0] : pal[idx];
				} else if (t.type == 3 || t.type == 11) {
					if (pos + (t.bpp == 16 ? 2 : 1) > n)
						return IMG_ERR_CORRUPT;
					px.r = px.g = px.b = d[pos++];
					px.a = 255;
					if (t.bpp == 16) {
						px.a = d[pos++];
						if (!alpha)
							px.a = 255;
					}
				} else if (t.bpp == 15 || t.bpp == 16) {
					unsigned int v;

					if (pos + 2 > n)
						return IMG_ERR_CORRUPT;
					v = img_le16(d + pos);
					pos += 2;
					px.b = (v & 31) * 255 / 31;
					px.g = ((v >> 5) & 31) * 255 / 31;
					px.r = ((v >> 10) & 31) * 255 / 31;
					px.a = (t.bpp == 16 && alpha && !(v & 0x8000)) ? 0 : 255;
				} else if (t.bpp == 24) {
					if (pos + 3 > n)
						return IMG_ERR_CORRUPT;
					px.b = d[pos++];
					px.g = d[pos++];
					px.r = d[pos++];
					px.a = 255;
				} else {
					if (pos + 4 > n)
						return IMG_ERR_CORRUPT;
					px.b = d[pos++];
					px.g = d[pos++];
					px.r = d[pos++];
					px.a = alpha ? d[pos] : 255;
					pos++;
				}
				out->px[oi] = img_rgb565(px.r, px.g, px.b);
				if (out->alpha)
					out->alpha[oi] = (unsigned char)px.a;
				if (++x >= t.w) {
					x = 0;
					y++;
				}
			}
		}
	}
	return y >= t.h ? 0 : IMG_ERR_CORRUPT;
}
