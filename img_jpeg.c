/* JPEG: baseline and extended-sequential 8-bit Huffman JPEG.
 *
 * Progressive/arithmetic JPEG and CMYK are rejected with IMG_ERR_UNSUPPORTED.
 * Decoding uses full component planes (Y full size, chroma subsampled), which
 * keeps the code simple and still fits the viewer's arena for normal photos. */

#include "img_internal.h"

#ifdef IMG_HOST_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do { } while (0)
#endif

static const unsigned char zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

#define CONST_BITS 13
#define PASS1_BITS 2
#define FIX_0_298631336  2446
#define FIX_0_390180644  3196
#define FIX_0_541196100  4433
#define FIX_0_765366865  6270
#define FIX_0_899976223  7373
#define FIX_1_175875602  9633
#define FIX_1_501321110  12299
#define FIX_1_847759065  15137
#define FIX_1_961570560  16069
#define FIX_2_053119869  16819
#define FIX_2_562915447  20995
#define FIX_3_072711026  25172

static int descale(int x, int n)
{
	return (x + (1 << (n - 1))) >> n;
}

static void idct_block(const int *in, unsigned char *out)
{
	int ws[64];
	int i, n;

	for (i = 0; i < 8; i++) {
		if (in[8 + i] == 0 && in[16 + i] == 0 && in[24 + i] == 0 &&
		    in[32 + i] == 0 && in[40 + i] == 0 && in[48 + i] == 0 &&
		    in[56 + i] == 0) {
			int dc = in[i] * (1 << PASS1_BITS);

			for (n = 0; n < 8; n++)
				ws[i + n * 8] = dc;
			continue;
		}
		{
			int z1, z2, z3, z4, z5;
			int tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;

			z2 = in[16 + i];
			z3 = in[48 + i];
			z1 = (z2 + z3) * FIX_0_541196100;
			tmp2 = z1 + z3 * -FIX_1_847759065;
			tmp3 = z1 + z2 * FIX_0_765366865;

			z2 = in[i];
			z3 = in[32 + i];
			tmp0 = (z2 + z3) * (1 << CONST_BITS);
			tmp1 = (z2 - z3) * (1 << CONST_BITS);

			tmp10 = tmp0 + tmp3;
			tmp13 = tmp0 - tmp3;
			tmp11 = tmp1 + tmp2;
			tmp12 = tmp1 - tmp2;

			tmp0 = in[56 + i];
			tmp1 = in[40 + i];
			tmp2 = in[24 + i];
			tmp3 = in[8 + i];

			z1 = tmp0 + tmp3;
			z2 = tmp1 + tmp2;
			z3 = tmp0 + tmp2;
			z4 = tmp1 + tmp3;
			z5 = (z3 + z4) * FIX_1_175875602;

			tmp0 *= FIX_0_298631336;
			tmp1 *= FIX_2_053119869;
			tmp2 *= FIX_3_072711026;
			tmp3 *= FIX_1_501321110;
			z1 *= -FIX_0_899976223;
			z2 *= -FIX_2_562915447;
			z3 *= -FIX_1_961570560;
			z4 *= -FIX_0_390180644;

			z3 += z5;
			z4 += z5;

			tmp0 += z1 + z3;
			tmp1 += z2 + z4;
			tmp2 += z2 + z3;
			tmp3 += z1 + z4;

			ws[i + 0 * 8] = descale(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
			ws[i + 7 * 8] = descale(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
			ws[i + 1 * 8] = descale(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
			ws[i + 6 * 8] = descale(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
			ws[i + 2 * 8] = descale(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
			ws[i + 5 * 8] = descale(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
			ws[i + 3 * 8] = descale(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
			ws[i + 4 * 8] = descale(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
		}
	}

	for (i = 0; i < 8; i++) {
		int *w = ws + i * 8;

		if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 &&
		    w[5] == 0 && w[6] == 0 && w[7] == 0) {
			int dc = descale(w[0], PASS1_BITS + 3) + 128;

			if (dc < 0) dc = 0;
			if (dc > 255) dc = 255;
			for (n = 0; n < 8; n++)
				out[i * 8 + n] = (unsigned char)dc;
			continue;
		}
		{
			int z1, z2, z3, z4, z5;
			int tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13;

			z2 = w[2];
			z3 = w[6];
			z1 = (z2 + z3) * FIX_0_541196100;
			tmp2 = z1 + z3 * -FIX_1_847759065;
			tmp3 = z1 + z2 * FIX_0_765366865;

			tmp0 = (w[0] + w[4]) * (1 << CONST_BITS);
			tmp1 = (w[0] - w[4]) * (1 << CONST_BITS);

			tmp10 = tmp0 + tmp3;
			tmp13 = tmp0 - tmp3;
			tmp11 = tmp1 + tmp2;
			tmp12 = tmp1 - tmp2;

			tmp0 = w[7];
			tmp1 = w[5];
			tmp2 = w[3];
			tmp3 = w[1];

			z1 = tmp0 + tmp3;
			z2 = tmp1 + tmp2;
			z3 = tmp0 + tmp2;
			z4 = tmp1 + tmp3;
			z5 = (z3 + z4) * FIX_1_175875602;

			tmp0 *= FIX_0_298631336;
			tmp1 *= FIX_2_053119869;
			tmp2 *= FIX_3_072711026;
			tmp3 *= FIX_1_501321110;
			z1 *= -FIX_0_899976223;
			z2 *= -FIX_2_562915447;
			z3 *= -FIX_1_961570560;
			z4 *= -FIX_0_390180644;

			z3 += z5;
			z4 += z5;

			tmp0 += z1 + z3;
			tmp1 += z2 + z4;
			tmp2 += z2 + z3;
			tmp3 += z1 + z4;

#define PUT(idx, val) do { \
	int v = descale((val), CONST_BITS + PASS1_BITS + 3) + 128; \
	if (v < 0) v = 0; \
	if (v > 255) v = 255; \
	out[i * 8 + (idx)] = (unsigned char)v; \
} while (0)
			PUT(0, tmp10 + tmp3);
			PUT(7, tmp10 - tmp3);
			PUT(1, tmp11 + tmp2);
			PUT(6, tmp11 - tmp2);
			PUT(2, tmp12 + tmp1);
			PUT(5, tmp12 - tmp1);
			PUT(3, tmp13 + tmp0);
			PUT(4, tmp13 - tmp0);
#undef PUT
		}
	}
}

struct jpeg_huff {
	unsigned char bits[17];
	unsigned char vals[256];
	int mincode[17];
	int maxcode[18];
	int valptr[17];
	int total;
};

struct jpeg_comp {
	unsigned char id, h, v, tq;
	int td, ta;
	unsigned char *plane;
	unsigned int pw, ph;
	int dc;
};

struct jpeg {
	const unsigned char *d;
	unsigned int n, pos;
	unsigned int w, h;
	unsigned int ncomp;
	struct jpeg_comp comp[3];
	unsigned int max_h, max_v;
	unsigned int mcus_x, mcus_y;
	unsigned char qt[4][64];       /* zigzag order */
	struct jpeg_huff dc[4], ac[4];
	unsigned int restart;
	unsigned int rst_next;
	unsigned int bitbuf;
	int bitcnt;
	unsigned int marker;
	int eof;
#ifdef IMG_HOST_DEBUG
	unsigned int dbg;
#endif
};

static int jpeg_u8(struct jpeg *j, unsigned int *v)
{
	if (j->pos >= j->n)
		return -1;
	*v = j->d[j->pos++];
	return 0;
}

static int jpeg_u16(struct jpeg *j, unsigned int *v)
{
	unsigned int a, b;

	if (jpeg_u8(j, &a) || jpeg_u8(j, &b))
		return -1;
	*v = (a << 8) | b;
	return 0;
}

static int jpeg_marker(struct jpeg *j)
{
	if (j->pos >= j->n || j->d[j->pos++] != 0xFF)
		return -1;
	while (j->pos < j->n && j->d[j->pos] == 0xFF)
		j->pos++;
	if (j->pos >= j->n)
		return -1;
	return (int)j->d[j->pos++];
}

static int huff_build(struct jpeg_huff *h, const unsigned int *counts,
		      const unsigned char *vals)
{
	int code = 0, k = 0, l, total = 0;

	for (l = 1; l <= 16; l++) {
		h->bits[l] = (unsigned char)counts[l - 1];
		total += (int)counts[l - 1];
	}
	h->total = 0;
	if (total > 256)
		return -1;
	for (l = 0; l < total; l++)
		h->vals[l] = vals[l];
	{
		int left = 1;

		for (l = 1; l <= 16; l++) {
			left <<= 1;
			left -= (int)counts[l - 1];
			if (left < 0)
				return -1;
		}
	}
	k = 0;
	for (l = 1; l <= 16; l++) {
		if (h->bits[l]) {
			h->valptr[l] = k;
			h->mincode[l] = code;
			code += h->bits[l];
			h->maxcode[l] = code - 1;
			k += h->bits[l];
		} else {
			h->valptr[l] = 0;
			h->mincode[l] = 0;
			h->maxcode[l] = -1;
		}
		code <<= 1;
	}
	h->maxcode[17] = 0x7FFFFFFF;
	h->total = total;
	return 0;
}

static int huff_decode(struct jpeg *j, const struct jpeg_huff *h)
{
	int code = 0, l;

	if (h->total <= 0)
		return -1;

	for (l = 1; l <= 16; l++) {
		int b;

		if (j->bitcnt == 0) {
			unsigned int v;

			if (j->pos >= j->n)
				return -1;
			v = j->d[j->pos++];
			if (v == 0xFF) {
				unsigned int v2;

				if (j->pos >= j->n)
					return -1;
				v2 = j->d[j->pos++];
				if (v2 != 0x00) {
					j->marker = 0xFF00 | v2;
					return -1;
				}
				v = 0xFF;
			}
			j->bitbuf = v;
			j->bitcnt = 8;
		}
		b = (int)((j->bitbuf >> (j->bitcnt - 1)) & 1);
		j->bitcnt--;
		code = (code << 1) | b;
		if (h->maxcode[l] >= 0 && code >= h->mincode[l] &&
		    code <= h->maxcode[l]) {
			int idx = h->valptr[l] + code - h->mincode[l];

			if (idx < 0 || idx >= h->total)
				return -1;
			return h->vals[idx];
		}
	}
	return -1;
}

static int jpeg_bits(struct jpeg *j, int n)
{
	int v = 0;

	while (n--) {
		int b;

		if (j->bitcnt == 0) {
			unsigned int x;

			if (j->pos >= j->n)
				return -1;
			x = j->d[j->pos++];
			if (x == 0xFF) {
				unsigned int x2;

				if (j->pos >= j->n)
					return -1;
				x2 = j->d[j->pos++];
				if (x2 != 0x00) {
					j->marker = 0xFF00 | x2;
					return -1;
				}
				x = 0xFF;
			}
			j->bitbuf = x;
			j->bitcnt = 8;
		}
		b = (int)((j->bitbuf >> (j->bitcnt - 1)) & 1);
		j->bitcnt--;
		v = (v << 1) | b;
	}
	return v;
}

static int jpeg_extend(int v, int n)
{
	if (n == 0)
		return 0;
	return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

static int jpeg_restart(struct jpeg *j)
{
	j->bitcnt = 0;
	while (j->pos + 1 < j->n) {
		if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0x00) {
			unsigned int m = j->d[j->pos + 1];

			j->pos += 2;
			if (m == 0xD0 + (j->rst_next & 7)) {
				j->rst_next++;
				return 0;
			}
			return -1;
		}
		j->pos++;
	}
	return -1;
}

/* Bilinear chroma sample at a fixed-point position (16.16) in the component
 * plane.  The half-sample offset keeps the chroma centred on its luma block. */
static int jpeg_chroma(const struct jpeg_comp *c, int posx, int posy)
{
	int x0, y0, x1, y1, fx, fy;
	int v00, v10, v01, v11;

	if (posx < 0) {
		x0 = x1 = 0;
		fx = 0;
	} else {
		x0 = posx >> 16;
		fx = posx & 65535;
		if (x0 >= (int)c->pw) {
			x0 = x1 = (int)c->pw - 1;
			fx = 0;
		} else {
			x1 = x0 + 1;
			if (x1 >= (int)c->pw)
				x1 = (int)c->pw - 1;
		}
	}
	if (posy < 0) {
		y0 = y1 = 0;
		fy = 0;
	} else {
		y0 = posy >> 16;
		fy = posy & 65535;
		if (y0 >= (int)c->ph) {
			y0 = y1 = (int)c->ph - 1;
			fy = 0;
		} else {
			y1 = y0 + 1;
			if (y1 >= (int)c->ph)
				y1 = (int)c->ph - 1;
		}
	}

	v00 = c->plane[y0 * c->pw + x0];
	v10 = c->plane[y0 * c->pw + x1];
	v01 = c->plane[y1 * c->pw + x0];
	v11 = c->plane[y1 * c->pw + x1];
	return ((v00 * (65536 - fx) + v10 * fx) >> 16) * (65536 - fy) / 65536 +
	       ((v01 * (65536 - fx) + v11 * fx) >> 16) * fy / 65536;
}

static int jpeg_block(struct jpeg *j, struct jpeg_comp *c, int *blk)
{
	const unsigned char *q = j->qt[c->tq];
	int k, t;

	for (k = 0; k < 64; k++)
		blk[k] = 0;

	t = huff_decode(j, &j->dc[c->td]);
#ifdef IMG_HOST_DEBUG
	if (j->dbg < 4)
		DBG("jpeg block comp %u dc t=%d pos=%u bitcnt=%d\n",
		    c->id, t, j->pos, j->bitcnt);
	j->dbg++;
#endif
	if (t < 0 || t > 15)
		return -1;
	if (t) {
		int v = jpeg_bits(j, t);

		if (v < 0)
			return -1;
		c->dc += jpeg_extend(v, t);
	}
	blk[0] = c->dc * q[0];

	for (k = 1; k < 64; ) {
		int rs = huff_decode(j, &j->ac[c->ta]);
		int r, s;

		if (rs < 0)
			return -1;
		r = rs >> 4;
		s = rs & 15;
		if (s == 0) {
			if (r == 15) {
				k += 16;
				continue;
			}
			break;
		}
		k += r;
		if (k >= 64)
			return -1;
		{
			int v = jpeg_bits(j, s);

			if (v < 0)
				return -1;
			blk[zigzag[k]] = jpeg_extend(v, s) * q[k];
		}
		k++;
	}
	return 0;
}

static int jpeg_sof(struct jpeg *j, unsigned int marker)
{
	unsigned int len, prec, i;

	if (jpeg_u16(j, &len) || len < 8)
		return -1;
	if (jpeg_u8(j, &prec) || prec != 8)
		return IMG_ERR_UNSUPPORTED;
	if (jpeg_u16(j, &j->h) || jpeg_u16(j, &j->w))
		return -1;
	if (j->w == 0 || j->h == 0 || j->w > 32768 || j->h > 32768)
		return IMG_ERR_TOOBIG;
	if (jpeg_u8(j, &j->ncomp) ||
	    (j->ncomp != 1 && j->ncomp != 3))
		return IMG_ERR_UNSUPPORTED;
	if (len < 8 + j->ncomp * 3)
		return -1;
	if (marker == 0xC2 || marker == 0xC6 || marker == 0xCA ||
	    marker == 0xCE)
		return IMG_ERR_UNSUPPORTED;
	j->max_h = j->max_v = 1;
	for (i = 0; i < j->ncomp; i++) {
		unsigned int id, hv, tq;

		if (jpeg_u8(j, &id) || jpeg_u8(j, &hv) || jpeg_u8(j, &tq))
			return -1;
		j->comp[i].id = (unsigned char)id;
		j->comp[i].h = (unsigned char)(hv >> 4);
		j->comp[i].v = (unsigned char)(hv & 15);
		j->comp[i].tq = (unsigned char)tq;
		if (j->comp[i].h == 0 || j->comp[i].v == 0 ||
		    j->comp[i].h > 4 || j->comp[i].v > 4 || tq > 3)
			return IMG_ERR_UNSUPPORTED;
		if (j->comp[i].h > j->max_h)
			j->max_h = j->comp[i].h;
		if (j->comp[i].v > j->max_v)
			j->max_v = j->comp[i].v;
	}
	j->mcus_x = (j->w + 8 * j->max_h - 1) / (8 * j->max_h);
	j->mcus_y = (j->h + 8 * j->max_v - 1) / (8 * j->max_v);
	return 0;
}

int img_sniff_jpeg(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int pos = 2;

	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
		return IMG_FMT_UNKNOWN;
	while (pos + 4 <= n) {
		unsigned int m;

		if (d[pos] != 0xFF) {
			pos++;
			continue;
		}
		while (pos < n && d[pos] == 0xFF)
			pos++;
		if (pos >= n)
			break;
		m = d[pos++];
		if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7))
			continue;
		if (pos + 2 > n)
			break;
		{
			unsigned int len = img_be16(d + pos);

			if (len < 2 || pos + len > n)
				break;
			if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 &&
			    m != 0xC8 && m != 0xCC) {
				out->format = IMG_FMT_JPEG;
				out->height = img_be16(d + pos + 3);
				out->width = img_be16(d + pos + 5);
				out->has_alpha = 0;
				return IMG_FMT_JPEG;
			}
			pos += len;
		}
	}
	return IMG_FMT_UNKNOWN;
}

int img_decode_jpeg(const unsigned char *d, unsigned int n,
		    const struct img_options *opt, struct img_mem *m,
		    struct img_image *out)
{
	struct jpeg *j;
	unsigned int mark, image_end, i;
	int rcode, sos = 0;

	mark = img_mem_mark(m);
	j = (struct jpeg *)img_mem_alloc(m, sizeof(*j), 16);
	if (!j)
		return IMG_ERR_NOMEM;
	for (i = 0; i < sizeof(*j); i++)
		((unsigned char *)j)[i] = 0;
	j->d = d;
	j->n = n;
	j->pos = 2;
	j->rst_next = 0;

	while (j->pos < n) {
		int mk = jpeg_marker(j);

		DBG("jpeg: marker %02X at %u ncomp %u\n", mk, j->pos, j->ncomp);
		if (mk < 0) {
			img_mem_rewind(m, mark);
			return IMG_ERR_CORRUPT;
		}
		if (mk == 0xD9)
			break;
		if (mk == 0xDA) {
			unsigned int len, ns, k;

			if (jpeg_u16(j, &len) || len < 6)
				goto bad;
			if (jpeg_u8(j, &ns) || ns != j->ncomp)
				goto unsupported;
			for (k = 0; k < ns; k++) {
				unsigned int id, tabs, c;

				if (jpeg_u8(j, &id) || jpeg_u8(j, &tabs))
					goto bad;
				if ((tabs >> 4) > 3 || (tabs & 15) > 3)
					goto bad;
				for (c = 0; c < j->ncomp; c++)
					if (j->comp[c].id == id)
						break;
				if (c == j->ncomp)
					goto bad;
				j->comp[c].td = (int)(tabs >> 4);
				j->comp[c].ta = (int)(tabs & 15);
			}
			/* Ss, Se, Ah/Al follow the component list. */
			j->pos += 3;
			sos = 1;
			break;
		}
		if (mk == 0xC0 || mk == 0xC1) {
			rcode = jpeg_sof(j, (unsigned int)mk);
			if (rcode)
				goto fail;
			if (!img_size_ok(j->w, j->h, opt))
				goto too_big;
			continue;
		}
		if (mk == 0xC2 || mk == 0xC6 || mk == 0xCA || mk == 0xCE) {
			rcode = IMG_ERR_UNSUPPORTED;
			goto fail;
		}
		{
			unsigned int len, k;

			if (jpeg_u16(j, &len) || len < 2 || j->pos + len - 2 > n)
				goto bad;
			if (mk == 0xDB) {         /* DQT */
				unsigned int end = j->pos + len - 2;

				while (j->pos < end) {
					unsigned int pqtq, pq, tq;

					if (jpeg_u8(j, &pqtq))
						goto bad;
					pq = pqtq >> 4;
					tq = pqtq & 15;
					if (pq > 1 || tq > 3)
						goto bad;
					for (k = 0; k < 64; k++) {
						unsigned int v;

						if (pq) {
							if (jpeg_u16(j, &v))
								goto bad;
							j->qt[tq][k] = (unsigned char)(v >> 8);
						} else {
							if (jpeg_u8(j, &v))
								goto bad;
							j->qt[tq][k] = (unsigned char)v;
						}
					}
				}
			} else if (mk == 0xC4) {  /* DHT */
				unsigned int end = j->pos + len - 2;

				while (j->pos < end) {
					unsigned int tcth, tc, th;
					unsigned int counts[16], total = 0;

					if (jpeg_u8(j, &tcth))
						goto bad;
					tc = tcth >> 4;
					th = tcth & 15;
					if (tc > 1 || th > 3)
						goto bad;
					for (k = 0; k < 16; k++) {
						unsigned int v;

						if (jpeg_u8(j, &v))
							goto bad;
						counts[k] = v;
						total += v;
					}
					DBG("jpeg: dht tc=%u th=%u total=%u c=%u,%u,%u,%u pos=%u end=%u\n",
					    tc, th, total, counts[0], counts[1],
					    counts[2], counts[3], j->pos, end);

					{
						unsigned char vals[256];

						if (total > 256)
							goto bad;
						for (k = 0; k < total; k++) {
							unsigned int v;

							if (jpeg_u8(j, &v))
								goto bad;
							vals[k] = (unsigned char)v;
						}
						if (tc)
							huff_build(&j->ac[th], counts, vals);
						else
							huff_build(&j->dc[th], counts, vals);
#ifdef IMG_HOST_DEBUG
						if (tc == 0 && th == 0) {
							int ll;

							for (ll = 1; ll <= 10; ll++)
								if (j->dc[0].bits[ll])
									DBG("  dc0 l=%d min=%d max=%d valptr=%d\n",
									    ll, j->dc[0].mincode[ll],
									    j->dc[0].maxcode[ll],
									    j->dc[0].valptr[ll]);
						}
#endif
					}
				}
			} else if (mk == 0xDD) {  /* DRI */
				if (jpeg_u16(j, &j->restart))
					goto bad;
				j->rst_next = 0;
			} else {
				j->pos += len - 2;
			}
		}
	}
	if (!sos)
		goto bad;
	if (!img_size_ok(j->w, j->h, opt))
		goto too_big;

	rcode = img_alloc(m, j->w, j->h, IMG_FMT_JPEG, 0, out);
	if (rcode)
		goto fail;
	image_end = img_mem_mark(m);

	/* Allocate padded component planes. */
	for (i = 0; i < j->ncomp; i++) {
		struct jpeg_comp *c = &j->comp[i];

		c->pw = j->mcus_x * c->h * 8;
		c->ph = j->mcus_y * c->v * 8;
		c->plane = (unsigned char *)img_mem_alloc(m, c->pw * c->ph, 16);
		if (!c->plane) {
			rcode = IMG_ERR_NOMEM;
			img_mem_rewind(m, mark);
			goto fail;
		}
		c->dc = 0;
	}

	/* Entropy-coded scan. */
	{
		unsigned int mx, my, mcu = 0;

		for (my = 0; my < j->mcus_y; my++) {
			for (mx = 0; mx < j->mcus_x; mx++) {
				unsigned int ci;

				for (ci = 0; ci < j->ncomp; ci++) {
					struct jpeg_comp *c = &j->comp[ci];
					unsigned int by, bx;

					for (by = 0; by < c->v; by++) {
						for (bx = 0; bx < c->h; bx++) {
							int blk[64];
							unsigned char p[64];
							unsigned int xx, yy;

							if (jpeg_block(j, c, blk))
								goto entropy_bad;
							idct_block(blk, p);
							for (yy = 0; yy < 8; yy++)
								for (xx = 0; xx < 8; xx++) {
									unsigned int px = mx * c->h * 8 + bx * 8 + xx;
									unsigned int py = my * c->v * 8 + by * 8 + yy;

									c->plane[py * c->pw + px] = p[yy * 8 + xx];
								}
						}
					}
				}
				mcu++;
				if (j->restart && (mcu % j->restart) == 0 &&
				    mcu < j->mcus_x * j->mcus_y) {
					for (ci = 0; ci < j->ncomp; ci++)
						j->comp[ci].dc = 0;
					if (jpeg_restart(j))
						goto entropy_bad;
				}
			}
		}
	}

	/* Convert planes to RGB565. */
	for (i = 0; i < j->h; i++) {
		unsigned int x;

		for (x = 0; x < j->w; x++) {
			struct jpeg_comp *y = &j->comp[0];
			unsigned int yy = y->plane[i * y->pw + x];
			unsigned int r, g, b;

			if (j->ncomp == 1) {
				r = g = b = yy;
			} else {
				struct jpeg_comp *cb = &j->comp[1];
				struct jpeg_comp *cr = &j->comp[2];
				unsigned int cstep_x, cstep_y, crstep_x, crstep_y;
				int cpos_x, cpos_y, crpos_x, crpos_y;
				int cbv, crv, yv;

				cstep_x = ((unsigned int)cb->h << 16) / j->max_h;
				cstep_y = ((unsigned int)cb->v << 16) / j->max_v;
				crstep_x = ((unsigned int)cr->h << 16) / j->max_h;
				crstep_y = ((unsigned int)cr->v << 16) / j->max_v;
				cpos_x = (int)(x * cstep_x) +
					(int)(((unsigned int)cb->h * 32768u) / j->max_h) -
					32768;
				cpos_y = (int)(i * cstep_y) +
					(int)(((unsigned int)cb->v * 32768u) / j->max_v) -
					32768;
				crpos_x = (int)(x * crstep_x) +
					(int)(((unsigned int)cr->h * 32768u) / j->max_h) -
					32768;
				crpos_y = (int)(i * crstep_y) +
					(int)(((unsigned int)cr->v * 32768u) / j->max_v) -
					32768;
				cbv = jpeg_chroma(cb, cpos_x, cpos_y) - 128;
				crv = jpeg_chroma(cr, crpos_x, crpos_y) - 128;
				yv = (int)yy;
				int rr = yv + ((91881 * crv) >> 16);
				int gg = yv - ((22554 * cbv + 46802 * crv) >> 16);
				int bb = yv + ((116130 * cbv) >> 16);

				if (rr < 0) rr = 0;
				if (gg < 0) gg = 0;
				if (bb < 0) bb = 0;
				if (rr > 255) rr = 255;
				if (gg > 255) gg = 255;
				if (bb > 255) bb = 255;
				r = (unsigned int)rr;
				g = (unsigned int)gg;
				b = (unsigned int)bb;
			}
			out->px[i * j->w + x] = img_rgb565(r, g, b);
		}
	}
	img_mem_rewind(m, image_end);
	return 0;

entropy_bad:
	img_mem_rewind(m, mark);
	rcode = IMG_ERR_CORRUPT;
	goto fail;
too_big:
	DBG("jpeg: too big %ux%u\n", j->w, j->h);
	rcode = IMG_ERR_TOOBIG;
	goto fail;
unsupported:
	DBG("jpeg: unsupported\n");
	rcode = IMG_ERR_UNSUPPORTED;
	goto fail;
bad:
	DBG("jpeg: bad at pos %u ncomp %u\n", j->pos, j->ncomp);
	rcode = IMG_ERR_CORRUPT;
fail:
	img_mem_rewind(m, mark);
	return rcode;
}
