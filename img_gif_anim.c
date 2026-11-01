/* Animated GIF frames: decode up to frame N, applying disposal methods. */

#include "img_internal.h"
struct ga_rd {
	const unsigned char *d;
	unsigned int n, pos, blk_left;
};

struct ga_rect {
	unsigned int x, y, w, h;
};

struct ga_lzw {
	struct ga_rd *r;
	unsigned int bitbuf;
	int bitcnt;
};

static int ga_u8(struct ga_rd *r, unsigned int *v)
{
	if (r->pos >= r->n)
		return -1;
	*v = r->d[r->pos++];
	return 0;
}

static int ga_subblock(struct ga_rd *r)
{
	unsigned int len;

	if (r->blk_left)
		return 0;
	if (ga_u8(r, &len))
		return -1;
	r->blk_left = len;
	return len ? 0 : 1;
}

static int ga_skip_subblocks(struct ga_rd *r)
{
	int e;

	do {
		e = ga_subblock(r);
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

static int ga_palette(const unsigned char *d, unsigned int n,
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

static int ga_header(const unsigned char *d, unsigned int n,
		     unsigned int *w, unsigned int *h, unsigned int *packed,
		     unsigned int *bg, unsigned int *goff, unsigned int *gent,
		     unsigned int *pos)
{
	if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F' ||
	    d[3] != '8' || (d[4] != '7' && d[4] != '9') || d[5] != 'a')
		return -1;
	*w = img_le16(d + 6);
	*h = img_le16(d + 8);
	*packed = d[10];
	*bg = d[11];
	*gent = 0;
	*goff = 13;
	if (*packed & 0x80) {
		*gent = 1u << ((*packed & 7) + 1);
		if (*goff + *gent * 3u > n)
			return -1;
	}
	*pos = *goff + *gent * 3u;
	return (*w && *h && *w <= 32768 && *h <= 32768) ? 0 : -1;
}

static unsigned int ga_scan_row(unsigned int fh, unsigned int n)
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

static int ga_code(struct ga_lzw *z, int bits, int *out)
{
	int v;

	while (z->bitcnt < bits) {
		int e = ga_subblock(z->r);
		unsigned int b;

		if (e < 0 || e > 0)
			return -1;
		if (ga_u8(z->r, &b))
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

/* Decode one frame's LZW stream into `idx` in display order. */
static int ga_decode_lzw(struct ga_rd *rd, unsigned int min_code,
			 unsigned int fw, unsigned int fh, int interlaced,
			 unsigned char *idx, struct img_mem *m)
{
	int *prefix;
	unsigned char *suffix, *stack;
	int clear = 1 << min_code;
	int end = clear + 1;
	int avail = end + 1;
	int code_size = (int)min_code + 1;
	int prev = -1;
	struct ga_lzw z;
	unsigned int done = 0;
	int rcode = -1;

	prefix = (int *)img_mem_alloc(m, 4096 * sizeof(int), 16);
	suffix = (unsigned char *)img_mem_alloc(m, 4096, 16);
	stack = (unsigned char *)img_mem_alloc(m, 4096, 16);
	if (!prefix || !suffix || !stack)
		return -1;
	{
		unsigned int i;

		for (i = 0; i < (unsigned int)clear; i++) {
			prefix[i] = -1;
			suffix[i] = (unsigned char)i;
		}
	}
	z.r = rd;
	z.bitbuf = 0;
	z.bitcnt = 0;

	for (;;) {
		int code, stack_len = 0, cur;
		unsigned int first;

		if (ga_code(&z, code_size, &code)) {
			if (done >= fw * fh) {
				rcode = 0;
				break;
			}
			goto out;
		}
		if (code == clear) {
			avail = end + 1;
			code_size = (int)min_code + 1;
			prev = -1;
			continue;
		}
		if (code == end) {
			rcode = 0;
			break;
		}
		if (prev < 0) {
			if (code < 0 || code >= clear)
				goto out;
			first = (unsigned int)code;
			if (done < fw * fh) {
				unsigned int fr = done / fw;
				unsigned int oy = interlaced ? ga_scan_row(fh, fr) : fr;

				idx[oy * fw + done % fw] = (unsigned char)code;
				done++;
			}
			prev = code;
			continue;
		}
		cur = code;
		if (code >= avail) {
			if (code != avail)
				goto out;
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
		while (stack_len > 0) {
			unsigned int v = stack[--stack_len];
			unsigned int fr, oy;

			if (done >= fw * fh)
				break;
			fr = done / fw;
			oy = interlaced ? ga_scan_row(fh, fr) : fr;
			idx[oy * fw + done % fw] = (unsigned char)v;
			done++;
		}
		prev = code;
		if (done >= fw * fh)
			break;
	}
out:
	rcode = (done >= fw * fh) ? 0 : -1;
	return rcode;
}

static void ga_fill(struct img_image *im, unsigned int x, unsigned int y,
		    unsigned int w, unsigned int h, unsigned int r,
		    unsigned int g, unsigned int b, unsigned int a)
{
	unsigned int i, j;

	if (x >= im->w || y >= im->h)
		return;
	if (x + w > im->w) w = im->w - x;
	if (y + h > im->h) h = im->h - y;
	for (j = 0; j < h; j++) {
		for (i = 0; i < w; i++) {
			unsigned int o = (y + j) * im->w + x + i;

			im->px[o] = img_rgb565(r, g, b);
			if (im->alpha)
				im->alpha[o] = (unsigned char)a;
		}
	}
}

static int ga_ensure_output(struct img_image *out, unsigned int w,
			    unsigned int h)
{
	if (out->px)
		return (out->w == w && out->h == h) ? 0 : -1;
	return -1;
}

int img_gif_frames(const void *data, unsigned int len, unsigned int *frames,
		   unsigned int *first_delay, int *has_alpha)
{
	const unsigned char *d = (const unsigned char *)data;
	unsigned int w, h, packed, bg, goff, gent, pos;
	struct ga_rd rd;
	unsigned int count = 0, delay = 0;
	int alpha = 0;

	if (ga_header(d, len, &w, &h, &packed, &bg, &goff, &gent, &pos))
		return -1;
	rd.d = d;
	rd.n = len;
	rd.pos = pos;
	rd.blk_left = 0;
	(void)w;
	(void)h;
	(void)packed;
	(void)bg;
	(void)goff;
	(void)gent;
	while (rd.pos < len) {
		unsigned int b;

		if (ga_u8(&rd, &b))
			break;
		if (b == 0x3B)
			break;
		if (b == 0x21) {
			unsigned int label;

			if (ga_u8(&rd, &label))
				break;
			if (label == 0xF9) {
				unsigned int l, i, pk = 0, dl = 0;

				if (ga_u8(&rd, &l) || l < 4)
					break;
				for (i = 0; i < l; i++) {
					unsigned int v;

					if (ga_u8(&rd, &v))
						break;
					if (i == 0) pk = v;
					if (i == 1) dl = v;
					if (i == 2) dl |= v << 8;
				}
				if (pk & 1) alpha = 1;
				if (!count && !delay) delay = dl;
				if (ga_u8(&rd, &b) || b != 0)
					break;
			} else if (ga_skip_subblocks(&rd)) {
				break;
			}
			continue;
		}
		if (b == 0x2C) {
			unsigned int fw, fh, fp, lct = 0;

			if (rd.pos + 9 > len)
				break;
			fw = img_le16(d + rd.pos + 4);
			fh = img_le16(d + rd.pos + 6);
			fp = d[rd.pos + 8];
			(void)fw;
			(void)fh;
			rd.pos += 9;
			if (fp & 0x80) {
				lct = 1u << ((fp & 7) + 1);
				rd.pos += lct * 3;
			}
			if (rd.pos >= len)
				break;
			rd.pos++;  /* min code size */
			if (ga_skip_subblocks(&rd))
				break;
			count++;
			continue;
		}
		if (b == 0x00) {
			while (rd.pos < len && d[rd.pos] == 0)
				rd.pos++;
			continue;
		}
		break;
	}
	if (frames) *frames = count;
	if (first_delay) *first_delay = delay ? delay : 10;
	if (has_alpha) *has_alpha = alpha;
	return count ? 0 : -1;
}

/* Decode frame `target` (0-based) into `out`.  The caller may pass a
 * preallocated image of the logical screen size for animation. */
int img_decode_gif_frame(const void *data, unsigned int len,
			 unsigned int target, struct img_mem *m,
			 struct img_image *out, unsigned int *delay_ms)
{
	const unsigned char *d = (const unsigned char *)data;
	unsigned int w, h, packed, bg, goff, gent, pos;
	unsigned char gpal[256][3], lpal[256][3];
	unsigned int lpal_n = 0;
	struct ga_rd rd;
	struct img_image *im = out;
	unsigned int frame = 0;
	unsigned int prev_disp = 0;
	struct ga_rect prev_rect = {0, 0, 0, 0};
	unsigned int cur_disp = 0, cur_trans = 0, cur_delay = 0;
	int cur_has_trans = 0;
	unsigned char *snapshot = 0;
	unsigned char *idx = 0;
	unsigned int idx_cap = 0;
	unsigned int entry_mark, scratch_mark;
	int rcode = -1;

	entry_mark = img_mem_mark(m);
	scratch_mark = entry_mark;
	if (ga_header(d, len, &w, &h, &packed, &bg, &goff, &gent, &pos))
		return IMG_ERR_FORMAT;
	for (frame = 0; frame < 256; frame++)
		gpal[frame][0] = gpal[frame][1] = gpal[frame][2] = 0;
	if (gent && ga_palette(d, len, goff, gent, gpal))
		return IMG_ERR_CORRUPT;
	if (!im->px) {
		int r = img_alloc(m, w, h, IMG_FMT_GIF, 1, im);
		if (r)
			return r;
	} else if (ga_ensure_output(im, w, h)) {
		return IMG_ERR_CORRUPT;
	}
	scratch_mark = img_mem_mark(m);
	ga_fill(im, 0, 0, w, h,
		gent && bg < gent ? gpal[bg][0] : 0,
		gent && bg < gent ? gpal[bg][1] : 0,
		gent && bg < gent ? gpal[bg][2] : 0, 255);

	rd.d = d;
	rd.n = len;
	rd.pos = pos;
	rd.blk_left = 0;
	frame = 0;
	while (rd.pos < len) {
		unsigned int b;

		if (ga_u8(&rd, &b))
			break;
		if (b == 0x3B)
			break;
		if (b == 0x21) {
			unsigned int label;

			if (ga_u8(&rd, &label))
				break;
			if (label == 0xF9) {
				unsigned int l, i, pk = 0, dl = 0, tr = 0;

				if (ga_u8(&rd, &l) || l < 4)
					break;
				for (i = 0; i < l; i++) {
					unsigned int v;

					if (ga_u8(&rd, &v))
						break;
					if (i == 0) pk = v;
					if (i == 1) dl = v;
					if (i == 2) dl |= v << 8;
					if (i == 3) tr = v;
				}
				cur_has_trans = (pk & 1) != 0;
				cur_disp = (pk >> 2) & 7;
				cur_delay = dl;
				cur_trans = tr;
				if (ga_u8(&rd, &b) || b != 0)
					break;
				continue;
			}
			if (ga_skip_subblocks(&rd))
				break;
			continue;
		}
		if (b == 0x2C) {
			unsigned int left, top, fw, fh, fp, min_code;
			unsigned int i, x, y;

			if (rd.pos + 9 > len)
				break;
			left = img_le16(d + rd.pos);
			top = img_le16(d + rd.pos + 2);
			fw = img_le16(d + rd.pos + 4);
			fh = img_le16(d + rd.pos + 6);
			fp = d[rd.pos + 8];
			(void)fw;
			(void)fh;
			rd.pos += 9;
			if (fw == 0 || fh == 0)
				break;
			lpal_n = 0;
			if (fp & 0x80) {
				lpal_n = 1u << ((fp & 7) + 1);
				if (ga_palette(d, len, rd.pos, lpal_n, lpal))
					break;
				rd.pos += lpal_n * 3u;
			}
			if (rd.pos >= len)
				break;
			min_code = d[rd.pos++];
			if (min_code < 2 || min_code > 8)
				break;

			if (frame > 0 && prev_disp == 2) {
				ga_fill(im, prev_rect.x, prev_rect.y,
					prev_rect.w, prev_rect.h,
					gent && bg < gent ? gpal[bg][0] : 0,
					gent && bg < gent ? gpal[bg][1] : 0,
					gent && bg < gent ? gpal[bg][2] : 0, 255);
			} else if (frame > 0 && prev_disp == 3 && snapshot) {
				for (i = 0; i < w * h; i++) {
					im->px[i] = ((unsigned short *)snapshot)[i];
					if (im->alpha)
						im->alpha[i] = snapshot[w * h * 2 + i];
				}
			}
			if (cur_disp == 3) {
				if (!snapshot) {
					snapshot = (unsigned char *)img_mem_alloc(m,
						w * h * 3u, 16);
					if (!snapshot)
						goto out;
				}
				for (i = 0; i < w * h; i++) {
					((unsigned short *)snapshot)[i] = im->px[i];
					if (im->alpha)
						snapshot[w * h * 2 + i] = im->alpha[i];
					else
						snapshot[w * h * 2 + i] = 255;
				}
			}
			if (fw * fh > idx_cap) {
				idx = (unsigned char *)img_mem_alloc(m, fw * fh, 16);
				if (!idx)
					goto out;
				idx_cap = fw * fh;
			}
			if (ga_decode_lzw(&rd, min_code, fw, fh,
					  (fp & 0x40) != 0, idx, m))
				goto out;
			if (ga_skip_subblocks(&rd))
				goto out;
			for (y = 0; y < fh; y++) {
				for (x = 0; x < fw; x++) {
					unsigned int oi = y * fw + x;
					unsigned int v = idx[oi];
					unsigned int ox = left + x;
					unsigned int oy = top + y;
					unsigned int o;

					if (cur_has_trans && v == cur_trans)
						continue;
					if (ox >= im->w || oy >= im->h)
						continue;
					o = oy * im->w + ox;
					if (lpal_n && v >= lpal_n)
						v = 0;
					if (lpal_n) {
						im->px[o] = img_rgb565(lpal[v][0],
							lpal[v][1], lpal[v][2]);
					} else {
						im->px[o] = img_rgb565(gpal[v][0],
							gpal[v][1], gpal[v][2]);
					}
					if (im->alpha)
						im->alpha[o] = 255;
				}
			}
			if (frame == target) {
				if (delay_ms)
					*delay_ms = cur_delay ? cur_delay * 10u : 100u;
				rcode = 0;
				goto out;
			}
			prev_disp = cur_disp;
			prev_rect.x = left;
			prev_rect.y = top;
			prev_rect.w = fw;
			prev_rect.h = fh;
			frame++;
			cur_has_trans = 0;
			cur_disp = 0;
			continue;
		}
		if (b == 0x00) {
			while (rd.pos < len && d[rd.pos] == 0)
				rd.pos++;
			continue;
		}
		break;
	}
out:
	img_mem_rewind(m, rcode == 0 ? scratch_mark : entry_mark);
	return rcode;
}
