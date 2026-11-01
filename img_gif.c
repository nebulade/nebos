/* GIF87a/89a: first frame, LZW, interlace and transparency. */

#include "img_internal.h"

struct gif_rd {
	const unsigned char *d;
	unsigned int n, pos;
	unsigned int blk_left;
};

static int gif_u8(struct gif_rd *r, unsigned int *v)
{
	if (r->pos >= r->n)
		return -1;
	*v = r->d[r->pos++];
	return 0;
}

static int gif_subblock(struct gif_rd *r)
{
	unsigned int len;

	if (r->blk_left)
		return 0;
	if (gif_u8(r, &len))
		return -1;
	r->blk_left = len;
	return len ? 0 : 1;      /* 1 = end of sub-block chain */
}

static int gif_skip_subblocks(struct gif_rd *r)
{
	int e;

	do {
		e = gif_subblock(r);
		if (e < 0)
			return -1;
		if (e > 0)
			return 0;
		while (r->blk_left) {
			r->blk_left--;
			if (r->pos >= r->n)
				return -1;
			r->pos++;
		}
	} while (1);
}

static int gif_palette(const unsigned char *d, unsigned int n,
		       unsigned int off, unsigned int entries,
		       unsigned char pal[256][3])
{
	unsigned int i;

	if (off > n || entries * 3u > n - off)
		return -1;
	for (i = 0; i < entries && i < 256; i++) {
		pal[i][0] = d[off + i * 3];
		pal[i][1] = d[off + i * 3 + 1];
		pal[i][2] = d[off + i * 3 + 2];
	}
	return 0;
}

static int gif_header(const unsigned char *d, unsigned int n,
		      unsigned int *w, unsigned int *h, unsigned int *packed,
		      unsigned int *bg, unsigned int *gct_off,
		      unsigned int *gct_entries, unsigned int *pos)
{
	if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F' ||
	    d[3] != '8' || (d[4] != '7' && d[4] != '9') || d[5] != 'a')
		return -1;
	*w = img_le16(d + 6);
	*h = img_le16(d + 8);
	*packed = d[10];
	*bg = d[11];
	*gct_entries = 0;
	*gct_off = 13;
	if (*packed & 0x80) {
		*gct_entries = 1u << ((*packed & 7) + 1);
		if (*gct_off + *gct_entries * 3u > n)
			return -1;
	}
	*pos = *gct_off + *gct_entries * 3u;
	if (*w == 0 || *h == 0 || *w > 32768 || *h > 32768)
		return -1;
	return 0;
}

/* Walk extensions until the first image descriptor.  Fills the transparency
 * information; returns 0 with the image descriptor position in *img_pos. */
static int gif_scan(const unsigned char *d, unsigned int n, unsigned int start,
		    unsigned int *img_pos, int *transparent,
		    unsigned int *trans_index)
{
	struct gif_rd r;

	r.d = d;
	r.n = n;
	r.pos = start;
	r.blk_left = 0;
	*transparent = 0;
	*trans_index = 0;
	while (r.pos < n) {
		unsigned int b;

		if (gif_u8(&r, &b))
			return -1;
		if (b == 0x3B) {
			*img_pos = r.pos;
			return 0;
		}
		if (b == 0x21) {
			unsigned int label;

			if (gif_u8(&r, &label))
				return -1;
			if (label == 0xF9) {
				unsigned int len, pk = 0, idx = 0;

				if (gif_u8(&r, &len) || len < 4)
					return -1;
				{
					unsigned int i;

					for (i = 0; i < len; i++) {
						unsigned int v;

						if (gif_u8(&r, &v))
							return -1;
						if (i == 0)
							pk = v;
						if (i == 3)
							idx = v;
					}
				}
				if (pk & 1) {
					*transparent = 1;
					*trans_index = idx;
				}
				if (gif_u8(&r, &b) || b != 0)
					return -1;   /* block terminator */
			} else {
				if (gif_skip_subblocks(&r))
					return -1;
			}
			continue;
		}
		if (b == 0x2C) {
			*img_pos = r.pos - 1;
			return 0;
		}
		if (b == 0x00) {
			while (r.pos < n && d[r.pos] == 0)
				r.pos++;
			continue;
		}
		return -1;
	}
	*img_pos = r.pos;
	return 0;
}

int img_sniff_gif(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int w, h, packed, bg, goff, gent, pos, ipos, ti;
	int tr;

	if (gif_header(d, n, &w, &h, &packed, &bg, &goff, &gent, &pos))
		return IMG_FMT_UNKNOWN;
	out->format = IMG_FMT_GIF;
	out->width = w;
	out->height = h;
	out->has_alpha = 0;
	if (gif_scan(d, n, pos, &ipos, &tr, &ti) == 0)
		out->has_alpha = tr != 0;
	return IMG_FMT_GIF;
}

struct gif_lzw {
	struct gif_rd *r;
	unsigned int bitbuf;
	int bitcnt;
};

static int lzw_code(struct gif_lzw *z, int bits, int *out)
{
	int v;

	while (z->bitcnt < bits) {
		int e = gif_subblock(z->r);
		unsigned int b;

		if (e < 0)
			return -1;
		if (e > 0)
			return -1;
		if (gif_u8(z->r, &b))
			return -1;
		z->r->blk_left--;
		z->bitbuf |= b << z->bitcnt;
		z->bitcnt += 8;
	}
	v = (int)(z->bitbuf & ((1u << bits) - 1u));
	z->bitbuf >>= bits;
	z->bitcnt -= bits;
	*out = v;
	return 0;
}

static void gif_put(struct img_image *im, unsigned int x, unsigned int y,
		    unsigned int r, unsigned int g, unsigned int b,
		    unsigned int a)
{
	if (x >= im->w || y >= im->h)
		return;
	im->px[y * im->w + x] = img_rgb565(r, g, b);
	if (im->alpha)
		im->alpha[y * im->w + x] = (unsigned char)a;
}

static unsigned int gif_scan_row(unsigned int fh, unsigned int n)
{
	static const unsigned int start[4] = {0, 4, 2, 1};
	static const unsigned int step[4] = {8, 8, 4, 2};
	unsigned int p;

	for (p = 0; p < 4; p++) {
		unsigned int count = 0;

		if (fh > start[p])
			count = (fh - start[p] + step[p] - 1) / step[p];
		if (n < count)
			return start[p] + n * step[p];
		n -= count;
	}
	return fh ? fh - 1 : 0;
}

int img_decode_gif(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	unsigned int w, h, packed, bg, goff, gent, pos, ipos;
	unsigned int trans_index = 0;
	int transparent = 0;
	unsigned char gpal[256][3], lpal[256][3];
	unsigned int fw, fh, left, top, fpacked, lct_entries = 0;
	unsigned int min_code, i;
	unsigned int total_pixels, done = 0;
	unsigned int mark, image_end;
	struct gif_rd rd;
	struct gif_lzw z;
	int rcode;

	mark = img_mem_mark(m);
	if (gif_header(d, n, &w, &h, &packed, &bg, &goff, &gent, &pos))
		return IMG_ERR_FORMAT;
	if (!img_size_ok(w, h, opt))
		return IMG_ERR_TOOBIG;
	for (i = 0; i < 256; i++)
		gpal[i][0] = gpal[i][1] = gpal[i][2] = 0;
	if (gent && gif_palette(d, n, goff, gent, gpal))
		return IMG_ERR_CORRUPT;
	if (gif_scan(d, n, pos, &ipos, &transparent, &trans_index))
		return IMG_ERR_CORRUPT;
	if (ipos + 9 > n || d[ipos] != 0x2C)
		return IMG_ERR_CORRUPT;
	left = img_le16(d + ipos + 1);
	top = img_le16(d + ipos + 3);
	fw = img_le16(d + ipos + 5);
	fh = img_le16(d + ipos + 7);
	fpacked = d[ipos + 9];
	pos = ipos + 10;
	if (fw == 0 || fh == 0)
		return IMG_ERR_CORRUPT;
	if (left > w || top > h || fw > w - left || fh > h - top)
		return IMG_ERR_UNSUPPORTED;
	if (fpacked & 0x80) {
		lct_entries = 1u << ((fpacked & 7) + 1);
		if (gif_palette(d, n, pos, lct_entries, lpal))
			return IMG_ERR_CORRUPT;
		pos += lct_entries * 3u;
	} else {
		lct_entries = gent;
		for (i = 0; i < 256; i++) {
			lpal[i][0] = gpal[i][0];
			lpal[i][1] = gpal[i][1];
			lpal[i][2] = gpal[i][2];
		}
	}
	(void)lct_entries;
	if (pos >= n)
		return IMG_ERR_CORRUPT;
	min_code = d[pos++];
	if (min_code < 2 || min_code > 8)
		return IMG_ERR_UNSUPPORTED;

	rcode = img_alloc(m, w, h, IMG_FMT_GIF, transparent, out);
	if (rcode)
		return rcode;
	image_end = img_mem_mark(m);
	if (gent && bg < gent) {
		for (i = 0; i < w * h; i++)
			out->px[i] = img_rgb565(gpal[bg][0], gpal[bg][1], gpal[bg][2]);
	} else {
		for (i = 0; i < w * h; i++)
			out->px[i] = 0;
	}
	if (out->alpha)
		for (i = 0; i < w * h; i++)
			out->alpha[i] = 255;

	rd.d = d;
	rd.n = n;
	rd.pos = pos;
	rd.blk_left = 0;
	z.r = &rd;
	z.bitbuf = 0;
	z.bitcnt = 0;
	total_pixels = fw * fh;

	{
		int *prefix;
		unsigned char *suffix, *stack;
		int clear = 1 << min_code;
		int end = clear + 1;
		int avail = end + 1;
		int code_size = (int)min_code + 1;
		int prev = -1;
		int interlaced = (fpacked & 0x40) != 0;

		prefix = (int *)img_mem_alloc(m, 4096 * sizeof(int), 16);
		suffix = (unsigned char *)img_mem_alloc(m, 4096, 16);
		stack = (unsigned char *)img_mem_alloc(m, 4096, 16);
		if (!prefix || !suffix || !stack) {
			img_mem_rewind(m, mark);
			return IMG_ERR_NOMEM;
		}
		for (i = 0; i < (unsigned int)clear; i++) {
			prefix[i] = -1;
			suffix[i] = (unsigned char)i;
		}

		for (;;) {
			int code;
			int stack_len = 0;
			unsigned int first;
			int cur;

			if (lzw_code(&z, code_size, &code)) {
				if (done >= total_pixels)
					break;
				img_mem_rewind(m, mark);
				return IMG_ERR_CORRUPT;
			}
			if (code == clear) {
				avail = end + 1;
				code_size = (int)min_code + 1;
				prev = -1;
				continue;
			}
			if (code == end)
				break;
			if (prev < 0) {
				if (code < 0 || code >= clear) {
					img_mem_rewind(m, mark);
					return IMG_ERR_CORRUPT;
				}
				first = (unsigned int)code;
				if (done < total_pixels) {
					unsigned int idx = (unsigned int)code;
					unsigned int fr = done / fw;
					unsigned int oy = top +
						(interlaced ? gif_scan_row(fh, fr) : fr);

					gif_put(out, left + (done % fw), oy,
						lpal[idx][0], lpal[idx][1],
						lpal[idx][2],
						(transparent && idx == trans_index) ? 0 : 255);
					done++;
				}
				prev = code;
				continue;
			}
			cur = code;
			if (code >= avail) {
				if (code != avail) {
					img_mem_rewind(m, mark);
					return IMG_ERR_CORRUPT;
				}
				stack[stack_len++] = (unsigned char)suffix[prev];
				cur = prev;
			}
			while (cur >= 0 && cur < avail && stack_len < 4096) {
				stack[stack_len++] = suffix[cur];
				cur = prefix[cur];
			}
			first = stack[stack_len - 1];
			if (avail < 4096) {
				prefix[avail] = prev;
				suffix[avail] = (unsigned char)first;
				avail++;
				if (avail == (1 << code_size) && code_size < 12)
					code_size++;
			}
			/* The stacked string was pushed in reverse order. */
			while (stack_len > 0) {
				unsigned int idx = stack[--stack_len];
				unsigned int fr, ox, oy;

				if (done >= total_pixels)
					break;
				fr = done / fw;
				ox = left + (done % fw);
				oy = top + (interlaced ? gif_scan_row(fh, fr) : fr);
				gif_put(out, ox, oy, lpal[idx][0], lpal[idx][1],
					lpal[idx][2],
					(transparent && idx == trans_index) ? 0 : 255);
				done++;
			}
			prev = code;
			if (done >= total_pixels)
				break;
		}
	}
	if (done < total_pixels) {
		img_mem_rewind(m, mark);
		return IMG_ERR_CORRUPT;
	}
	img_mem_rewind(m, image_end);
	return 0;
}
