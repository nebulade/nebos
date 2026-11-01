/* PNG: all standard colour types and bit depths, palette + tRNS, Adam7.
 *
 * The zlib stream is fed straight from consecutive IDAT chunks through
 * inflate.c's segmented source, so the compressed data is never copied. */

#include "img_internal.h"
#include "inflate.h"

#ifdef IMG_HOST_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

struct png_ctx {
	const unsigned char *d;
	unsigned int n;
	unsigned int w, h;
	unsigned int bit_depth, color_type, interlace;
	unsigned int channels;
	unsigned int has_trns;
	unsigned int trns_gray;
	unsigned int trns_r, trns_g, trns_b;
	unsigned char palette[256][3];
	unsigned char trns[256];
	unsigned int palette_entries;

	struct img_image *out;
	struct img_mem *mem;
	struct inflate *z;
	unsigned char *prev, *cur;
	unsigned int max_rowbytes;
	unsigned int filter_bpp;

	unsigned int pass;
	unsigned int pass_w, pass_h, pass_x0, pass_y0, pass_dx, pass_dy;
	unsigned int pass_row;
	unsigned int rowbytes;
	unsigned int rowpos;
	unsigned char *rowbuf;

	unsigned int finished;
	int err;
};

static const unsigned char pass_x0[7] = {0, 4, 0, 2, 0, 1, 0};
static const unsigned char pass_y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const unsigned char pass_dx[7] = {8, 8, 4, 4, 2, 2, 1};
static const unsigned char pass_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static int png_chunk_crc(const unsigned char *p, unsigned int len)
{
	unsigned int want = img_be32(p + 8 + len);
	unsigned int got = crc32_bytes(p + 4, len + 4);

	return want == got;
}

static void png_setup_pass(struct png_ctx *c)
{
	unsigned int pw, ph;

	if (c->interlace == 0) {
		unsigned int i;

		if (c->pass != 0) {
			c->finished = 1;
			return;
		}
		c->pass_w = c->w;
		c->pass_h = c->h;
		c->pass_x0 = 0;
		c->pass_y0 = 0;
		c->pass_dx = 1;
		c->pass_dy = 1;
		c->rowbytes = (c->w * c->channels * c->bit_depth + 7) / 8;
		c->pass_row = 0;
		c->rowpos = 0;
		for (i = 0; i < c->max_rowbytes; i++)
			c->prev[i] = 0;
		return;
	}

	while (c->pass < 7) {
		unsigned int x0 = pass_x0[c->pass], y0 = pass_y0[c->pass];
		unsigned int dx = pass_dx[c->pass], dy = pass_dy[c->pass];

		pw = c->w > x0 ? (c->w - x0 + dx - 1) / dx : 0;
		ph = c->h > y0 ? (c->h - y0 + dy - 1) / dy : 0;
		if (pw && ph) {
			c->pass_w = pw;
			c->pass_h = ph;
			c->pass_x0 = x0;
			c->pass_y0 = y0;
			c->pass_dx = dx;
			c->pass_dy = dy;
			c->rowbytes = (pw * c->channels * c->bit_depth + 7) / 8;
			c->pass_row = 0;
			c->rowpos = 0;
			{
				unsigned int i;

				for (i = 0; i < c->max_rowbytes; i++)
					c->prev[i] = 0;
			}
			return;
		}
		c->pass++;
	}
	c->finished = 1;
}

static unsigned int png_chan(const unsigned char *row, unsigned int px,
			     unsigned int ch_index, unsigned int nch,
			     unsigned int bd)
{
	unsigned int bits = nch * bd;
	unsigned int off = px * bits + ch_index * bd;

	if (bd == 8)
		return row[off >> 3];
	if (bd == 16)
		return ((unsigned int)row[off >> 3] << 8) | row[(off >> 3) + 1];
	{
		unsigned int byte = off >> 3;
		unsigned int shift = 8 - bd - (off & 7);
		return (row[byte] >> shift) & ((1u << bd) - 1u);
	}
}

static unsigned int png_scale(unsigned int v, unsigned int bd)
{
	if (bd == 8)
		return v;
	if (bd == 16)
		return v >> 8;
	return (v * 255u) / ((1u << bd) - 1u);
}

static int png_paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = p > a ? p - a : a - p;
	int pb = p > b ? p - b : b - p;
	int pc = p > c ? p - c : c - p;

	if (pa <= pb && pa <= pc)
		return a;
	if (pb <= pc)
		return b;
	return c;
}

static int png_row_done(struct png_ctx *c)
{
	unsigned int filter = c->rowbuf[0];
	unsigned int i;
	const unsigned char *src = c->rowbuf + 1;
	unsigned char *dst = c->cur;

	if (filter > 4) {
		DBG("png: bad filter %u\n", filter);
		return -1;
	}
	for (i = 0; i < c->rowbytes; i++) {
		int a = i >= c->filter_bpp ? dst[i - c->filter_bpp] : 0;
		int b = c->prev[i];
		int cc = i >= c->filter_bpp ? c->prev[i - c->filter_bpp] : 0;
		int x = src[i];

		switch (filter) {
		case 0: break;
		case 1: x += a; break;
		case 2: x += b; break;
		case 3: x += (a + b) >> 1; break;
		case 4: x += png_paeth(a, b, cc); break;
		}
		dst[i] = (unsigned char)x;
	}

	/* Map this pass row into the output image. */
	{
		unsigned int px;
		unsigned int oy = c->pass_y0 + c->pass_row * c->pass_dy;

		for (px = 0; px < c->pass_w; px++) {
			unsigned int ox = c->pass_x0 + px * c->pass_dx;
			unsigned int oi = oy * c->w + ox;
			unsigned int r = 0, g = 0, b = 0, a = 255;
			unsigned int bd = c->bit_depth;

			if (c->color_type == 0) {
				unsigned int raw = png_chan(dst, px, 0, 1, bd);
				unsigned int gray = png_scale(raw, bd);

				r = g = b = gray;
				if (c->has_trns && raw == c->trns_gray)
					a = 0;
			} else if (c->color_type == 2) {
				unsigned int rr = png_chan(dst, px, 0, 3, bd);
				unsigned int gg = png_chan(dst, px, 1, 3, bd);
				unsigned int bb = png_chan(dst, px, 2, 3, bd);

				r = png_scale(rr, bd);
				g = png_scale(gg, bd);
				b = png_scale(bb, bd);
				if (c->has_trns && rr == c->trns_r &&
				    gg == c->trns_g && bb == c->trns_b)
					a = 0;
			} else if (c->color_type == 3) {
				unsigned int idx = png_chan(dst, px, 0, 1, bd);

				if (idx >= c->palette_entries) {
					DBG("png: palette idx %u >= %u\n", idx,
					    c->palette_entries);
					return -1;
				}
				r = c->palette[idx][0];
				g = c->palette[idx][1];
				b = c->palette[idx][2];
				if (c->has_trns)
					a = c->trns[idx];
			} else if (c->color_type == 4) {
				unsigned int raw = png_chan(dst, px, 0, 2, bd);
				unsigned int ar = png_chan(dst, px, 1, 2, bd);

				r = g = b = png_scale(raw, bd);
				a = png_scale(ar, bd);
			} else if (c->color_type == 6) {
				unsigned int rr = png_chan(dst, px, 0, 4, bd);
				unsigned int gg = png_chan(dst, px, 1, 4, bd);
				unsigned int bb = png_chan(dst, px, 2, 4, bd);
				unsigned int aa = png_chan(dst, px, 3, 4, bd);

				r = png_scale(rr, bd);
				g = png_scale(gg, bd);
				b = png_scale(bb, bd);
				a = png_scale(aa, bd);
			}
			c->out->px[oi] = img_rgb565(r, g, b);
			if (c->out->alpha)
				c->out->alpha[oi] = (unsigned char)a;
		}
	}

	{
		unsigned char *tmp = c->prev;

		c->prev = c->cur;
		c->cur = tmp;
	}
	c->pass_row++;
	if (c->pass_row >= c->pass_h) {
		c->pass++;
		png_setup_pass(c);
	}
	return 0;
}

static int png_sink_write(void *ctx, const unsigned char *data, unsigned int len)
{
	struct png_ctx *c = (struct png_ctx *)ctx;

	while (len--) {
		if (c->finished)
			return -1;
		c->rowbuf[c->rowpos++] = *data++;
		if (c->rowpos == c->rowbytes + 1) {
			c->rowpos = 0;
			if (png_row_done(c))
				return -1;
		}
	}
	return 0;
}

/* IDAT walker: serves consecutive IDAT payloads and verifies their CRCs. */
struct png_idat {
	const unsigned char *p;
	const unsigned char *end;
	const unsigned char *cur;
	unsigned int cur_len;
	int err;
};

static int png_idat_more(void *ctx, const unsigned char **out, unsigned int *len)
{
	struct png_idat *c = (struct png_idat *)ctx;

	while (c->cur_len == 0) {
		unsigned int l;

		if (c->p + 12 > c->end) {
			DBG("png: idat: no more header\n");
			return 0;
		}
		l = img_be32(c->p);
		if (l > (unsigned int)(c->end - (c->p + 12))) {
			DBG("png: idat: bad len %u\n", l);
			return 0;
		}
		if (c->p[4] != 'I' || c->p[5] != 'D' || c->p[6] != 'A' ||
		    c->p[7] != 'T') {
			DBG("png: idat: next type %c%c%c%c len %u\n",
			    c->p[4], c->p[5], c->p[6], c->p[7], l);
			return 0;
		}
		if (!png_chunk_crc(c->p, l)) {
			DBG("png: idat: bad crc len %u\n", l);
			c->err = 1;
			return 0;
		}
		DBG("png: idat chunk len %u at %u\n", l,
		    (unsigned)(c->p - c->end));
		c->cur = c->p + 8;
		c->cur_len = l;
		c->p += 12 + l;
	}
	*out = c->cur;
	*len = c->cur_len;
	c->cur_len = 0;
	return 1;
}

int img_sniff_png(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int pos = 8;
	int trns = 0;

	if (n < 33)
		return IMG_FMT_UNKNOWN;
	if (img_be32(d + 8) != 13 || d[12] != 'I' || d[13] != 'H' ||
	    d[14] != 'D' || d[15] != 'R')
		return IMG_FMT_UNKNOWN;
	out->format = IMG_FMT_PNG;
	out->width = img_be32(d + 16);
	out->height = img_be32(d + 20);
	{
		unsigned int ct = d[25];

		if (ct == 4 || ct == 6)
			trns = 1;
	}
	/* Find tRNS before the image data (small header scan). */
	while (pos + 12 <= n) {
		unsigned int l = img_be32(d + pos);

		if (l > n - pos - 12)
			break;
		if (d[pos + 4] == 't' && d[pos + 5] == 'R' &&
		    d[pos + 6] == 'N' && d[pos + 7] == 'S')
			trns = 1;
		if (d[pos + 4] == 'I' && d[pos + 5] == 'D' &&
		    d[pos + 6] == 'A' && d[pos + 7] == 'T')
			break;
		if (d[pos + 4] == 'I' && d[pos + 5] == 'E' &&
		    d[pos + 6] == 'N' && d[pos + 7] == 'D')
			break;
		pos += 12 + l;
	}
	out->has_alpha = trns;
	return IMG_FMT_PNG;
}

int img_decode_png(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	struct png_ctx c;
	struct png_idat idat;
	unsigned int pos, mark, image_end, zsize;
	unsigned int i;
	int have_plte = 0, have_idat = 0;
	int rcode;

	mark = img_mem_mark(m);
	for (i = 0; i < sizeof(c); i++)
		((unsigned char *)&c)[i] = 0;

	if (n < 33 || img_be32(d + 8) != 13 || d[12] != 'I' || d[13] != 'H' ||
	    d[14] != 'D' || d[15] != 'R' || !png_chunk_crc(d + 8, 13))
		return IMG_ERR_FORMAT;
	c.d = d;
	c.n = n;
	c.w = img_be32(d + 16);
	c.h = img_be32(d + 20);
	c.bit_depth = d[24];
	c.color_type = d[25];
	c.interlace = d[28];
	if (!img_size_ok(c.w, c.h, opt))
		return IMG_ERR_TOOBIG;
	if (d[26] != 0 || d[27] != 0 || c.interlace > 1)
		return IMG_ERR_UNSUPPORTED;
	if (c.color_type == 0) {
		if (c.bit_depth != 1 && c.bit_depth != 2 && c.bit_depth != 4 &&
		    c.bit_depth != 8 && c.bit_depth != 16)
			return IMG_ERR_UNSUPPORTED;
		c.channels = 1;
	} else if (c.color_type == 2) {
		if (c.bit_depth != 8 && c.bit_depth != 16)
			return IMG_ERR_UNSUPPORTED;
		c.channels = 3;
	} else if (c.color_type == 3) {
		if (c.bit_depth != 1 && c.bit_depth != 2 && c.bit_depth != 4 &&
		    c.bit_depth != 8)
			return IMG_ERR_UNSUPPORTED;
		c.channels = 1;
	} else if (c.color_type == 4) {
		if (c.bit_depth != 8 && c.bit_depth != 16)
			return IMG_ERR_UNSUPPORTED;
		c.channels = 2;
	} else if (c.color_type == 6) {
		if (c.bit_depth != 8 && c.bit_depth != 16)
			return IMG_ERR_UNSUPPORTED;
		c.channels = 4;
	} else {
		return IMG_ERR_UNSUPPORTED;
	}
	for (i = 0; i < 256; i++)
		c.trns[i] = 255;

	/* Pre-IDAT chunks. */
	pos = 33;
	while (pos + 12 <= n) {
		unsigned int l = img_be32(d + pos);
		const unsigned char *type = d + pos + 4;
		const unsigned char *data = d + pos + 8;

		if (l > n - pos - 12)
			return IMG_ERR_CORRUPT;
		if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' &&
		    type[3] == 'T') {
			have_idat = 1;
			break;
		}
		if (!png_chunk_crc(d + pos, l))
			return IMG_ERR_CORRUPT;
		if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' &&
		    type[3] == 'E') {
			if (l == 0 || l % 3 != 0 || l > 768)
				return IMG_ERR_CORRUPT;
			c.palette_entries = l / 3;
			for (i = 0; i < c.palette_entries; i++) {
				c.palette[i][0] = data[i * 3];
				c.palette[i][1] = data[i * 3 + 1];
				c.palette[i][2] = data[i * 3 + 2];
			}
			have_plte = 1;
		} else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' &&
			   type[3] == 'S') {
			c.has_trns = 1;
			if (c.color_type == 3) {
				if (l > c.palette_entries)
					return IMG_ERR_CORRUPT;
				for (i = 0; i < l; i++)
					c.trns[i] = data[i];
			} else if (c.color_type == 0) {
				if (l != 2)
					return IMG_ERR_CORRUPT;
				c.trns_gray = img_be16(data);
			} else if (c.color_type == 2) {
				if (l != 6)
					return IMG_ERR_CORRUPT;
				c.trns_r = img_be16(data);
				c.trns_g = img_be16(data + 2);
				c.trns_b = img_be16(data + 4);
			} else {
				return IMG_ERR_CORRUPT;
			}
		} else if (!(type[0] & 0x20)) {
			return IMG_ERR_UNSUPPORTED;   /* unknown critical chunk */
		}
		pos += 12 + l;
	}
	if (!have_idat)
		return IMG_ERR_CORRUPT;
	if (c.color_type == 3 && !have_plte)
		return IMG_ERR_CORRUPT;
	if (c.color_type == 3 && c.palette_entries == 0)
		return IMG_ERR_CORRUPT;

	rcode = img_alloc(m, c.w, c.h, IMG_FMT_PNG,
			  (c.color_type == 4 || c.color_type == 6 ||
			   c.has_trns) != 0, out);
	if (rcode)
		return rcode;
	image_end = img_mem_mark(m);
	c.out = out;
	c.mem = m;

	c.max_rowbytes = (c.w * c.channels * c.bit_depth + 7) / 8;
	c.filter_bpp = (c.channels * c.bit_depth + 7) / 8;
	if (c.filter_bpp < 1)
		c.filter_bpp = 1;
	c.rowbuf = (unsigned char *)img_mem_alloc(m, c.max_rowbytes + 1, 1);
	c.prev = (unsigned char *)img_mem_alloc(m, c.max_rowbytes, 1);
	c.cur = (unsigned char *)img_mem_alloc(m, c.max_rowbytes, 1);
	zsize = sizeof(struct inflate);
	c.z = (struct inflate *)img_mem_alloc(m, zsize, 16);
	if (!c.rowbuf || !c.prev || !c.cur || !c.z) {
		img_mem_rewind(m, mark);
		return IMG_ERR_NOMEM;
	}
	{
		unsigned int k;

		for (k = 0; k < c.max_rowbytes; k++)
			c.prev[k] = 0;
	}

	c.pass = 0;
	png_setup_pass(&c);

	/* Feed the IDAT stream. The first chunk is already located at pos. */
	idat.p = d + pos;
	idat.end = d + n;
	idat.cur = 0;
	idat.cur_len = 0;
	idat.err = 0;
	/* The first IDAT chunk begins at pos; prime it. */
	if (idat.p + 12 > idat.end)
		goto png_fail;
	{
		unsigned int l = img_be32(idat.p);

		if (l > (unsigned int)(idat.end - (idat.p + 12)))
			goto png_fail;
		if (idat.p[4] != 'I' || idat.p[5] != 'D' || idat.p[6] != 'A' ||
		    idat.p[7] != 'T')
			goto png_fail;
		if (!png_chunk_crc(idat.p, l))
			goto png_fail;
		idat.cur = idat.p + 8;
		idat.cur_len = l;
		idat.p += 12 + l;
	}

	/* The first segment is already handed over; subsequent IDAT chunks go
	 * through png_idat_more(). */
	{
		struct inflate_src src;
		struct inflate_sink sink;

		src.p = idat.cur;
		src.len = idat.cur_len;
		idat.cur_len = 0;   /* walker moves on when this is consumed */
		src.more = png_idat_more;
		src.ctx = &idat;
		sink.write = png_sink_write;
		sink.ctx = &c;
		inflate_init(c.z, &src, &sink);
	}
	DBG("png: inflate start at %u first=%u\n", (unsigned)(idat.cur - d),
	    (unsigned)(idat.p - d));
	if (inflate_zlib(c.z, 0) != 0) {
		DBG("png: inflate failed total=%u finished=%u rowpos=%u pass=%u row=%u/%u err=%d\n",
		    (unsigned)c.z->total, c.finished, c.rowpos, c.pass,
		    c.pass_row, c.pass_h, idat.err);
		img_mem_rewind(m, mark);
		return idat.err ? IMG_ERR_CORRUPT : IMG_ERR_CORRUPT;
	}
	DBG("png: inflate ok total=%u finished=%u rowpos=%u pass=%u\n",
	    (unsigned)c.z->total, c.finished, c.rowpos, c.pass);
	pos = (unsigned int)(idat.p - d);

	/* Expect IEND (ignore ancillary chunks). */
	while (pos + 12 <= n) {
		unsigned int l = img_be32(d + pos);
		const unsigned char *type = d + pos + 4;

		if (l > n - pos - 12)
			goto png_fail;
		if (!png_chunk_crc(d + pos, l))
			goto png_fail;
		if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' &&
		    type[3] == 'D') {
			pos += 12 + l;
			break;
		}
		pos += 12 + l;
	}
	if (!c.finished || c.rowpos != 0)
		goto png_fail;

	img_mem_rewind(m, image_end);
	return 0;

png_fail:
	img_mem_rewind(m, mark);
	return IMG_ERR_CORRUPT;
}
