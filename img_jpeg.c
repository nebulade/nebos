/* JPEG: baseline, extended-sequential and progressive 8-bit Huffman JPEG.
 *
 * Arithmetic-coded JPEG and CMYK are rejected with IMG_ERR_UNSUPPORTED.
 * Baseline scans decode straight into component planes; progressive scans
 * keep quantized coefficients until all scans have arrived, then dequantize
 * and IDCT into the same planes.  EXIF orientation is applied while writing
 * the RGB565 output. */

#include "img_internal.h"

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
	unsigned int bw, bh;      /* padded block grid (8x8 blocks) */
	unsigned int sw, sh;      /* actual block grid in a scan */
	unsigned int pw, ph;      /* padded plane size in pixels */
	short *coef;              /* progressive coefficients, zigzag order */
	unsigned char *plane;
	int dc;
};

struct jpeg {
	const unsigned char *d;
	unsigned int n, pos;
	unsigned int w, h;        /* raw dimensions */
	unsigned int out_w, out_h;/* after EXIF orientation */
	unsigned int ncomp;
	struct jpeg_comp comp[3];
	unsigned int max_h, max_v;
	unsigned int mcus_x, mcus_y;
	unsigned char qt[4][64];  /* zigzag order */
	struct jpeg_huff dc[4], ac[4];
	unsigned int restart;
	unsigned int rst_next;
	unsigned int bitbuf;
	int bitcnt;
	unsigned int marker;
	int progressive;
	unsigned int orientation;
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

	h->total = 0;
	for (l = 1; l <= 16; l++) {
		h->bits[l] = (unsigned char)counts[l - 1];
		total += (int)counts[l - 1];
	}
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
					j->pos -= 2;
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
					j->pos -= 2;
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

/* Skip to the next real marker after the entropy-coded scan. */
static void jpeg_scan_end(struct jpeg *j)
{
	j->bitcnt = 0;
	while (j->pos + 1 < j->n) {
		unsigned int b = j->d[j->pos];
		unsigned int m;

		if (b != 0xFF) {
			j->pos++;
			continue;
		}
		m = j->d[j->pos + 1];
		if (m == 0x00) {
			j->pos += 2;
			continue;
		}
		if (m >= 0xD0 && m <= 0xD7) {
			j->pos += 2;
			continue;
		}
		return;
	}
}

/* Bilinear chroma sample at a fixed-point position (16.16). */
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

/* Baseline sequential block: dequantize directly in natural order. */
static int jpeg_block(struct jpeg *j, struct jpeg_comp *c, int *blk)
{
	const unsigned char *q = j->qt[c->tq];
	int k, t;

	for (k = 0; k < 64; k++)
		blk[k] = 0;

	t = huff_decode(j, &j->dc[c->td]);
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

static int jpeg_refine_bit(struct jpeg *j, short *v, int Al)
{
	int bit = jpeg_bits(j, 1);
	int p1 = 1 << Al;

	if (bit < 0)
		return -1;
	if (bit && ((*v & p1) == 0)) {
		if (*v >= 0)
			*v = (short)(*v + p1);
		else
			*v = (short)(*v - p1);
	}
	return 0;
}

/* One 8x8 block of a progressive scan.  Coefficients are kept in zigzag
 * order; the first scan may use a nonzero Al bit position. */
static int jpeg_prog_block(struct jpeg *j, struct jpeg_comp *c, short *blk,
			   int Ss, int Se, int Ah, int Al, int *eobrun)
{
	int k, rs, r, s;

	if (Ss == 0) {
		if (Ah == 0) {
			int t = huff_decode(j, &j->dc[c->td]);
			int diff = 0;

			if (t < 0 || t > 15)
				return -1;
			if (t) {
				int v = jpeg_bits(j, t);

				if (v < 0)
					return -1;
				diff = jpeg_extend(v, t);
			}
			c->dc += diff;
			blk[0] = (short)(c->dc << Al);
		} else {
			int bit = jpeg_bits(j, 1);

			if (bit < 0)
				return -1;
			blk[0] = (short)(blk[0] | (bit << Al));
		}
		return 0;
	}

	if (Ah == 0) {
		if (*eobrun > 0) {
			(*eobrun)--;
			return 0;
		}
		k = Ss;
		while (k <= Se) {
			rs = huff_decode(j, &j->ac[c->ta]);
			if (rs < 0)
				return -1;
			r = rs >> 4;
			s = rs & 15;
			if (s == 0) {
				if (r == 15) {
					k += 16;
					continue;
				}
				*eobrun = (1 << r) - 1;
				if (r) {
					int v = jpeg_bits(j, r);

					if (v < 0)
						return -1;
					*eobrun += v;
				}
				break;
			}
			k += r;
			if (k > Se)
				return -1;
			{
				int v = jpeg_bits(j, s);

				if (v < 0)
					return -1;
				blk[k] = (short)(jpeg_extend(v, s) << Al);
			}
			k++;
		}
		return 0;
	}

	/* AC successive-approximation refinement. */
	k = Ss;
	if (*eobrun == 0) {
		for (; k <= Se; k++) {
			int newval = 0;
			int p1 = 1 << Al;

			rs = huff_decode(j, &j->ac[c->ta]);
			if (rs < 0)
				return -1;
			r = rs >> 4;
			s = rs & 15;
			if (s) {
				int bit;

				if (s != 1)
					return -1;
				bit = jpeg_bits(j, 1);
				if (bit < 0)
					return -1;
				newval = bit ? p1 : -p1;
			} else {
				if (r != 15) {
					*eobrun = 1 << r;
					if (r) {
						int v = jpeg_bits(j, r);

						if (v < 0)
							return -1;
						*eobrun += v;
					}
					break;
				}
			}
			do {
				if (blk[k]) {
					if (jpeg_refine_bit(j, &blk[k], Al))
						return -1;
				} else {
					if (--r < 0)
						break;
				}
				k++;
			} while (k <= Se);
			if (newval) {
				if (k > Se)
					return -1;
				blk[k] = (short)newval;
			}
		}
	}
	if (*eobrun > 0) {
		for (; k <= Se; k++)
			if (blk[k])
				if (jpeg_refine_bit(j, &blk[k], Al))
					return -1;
		(*eobrun)--;
	}

	return 0;
}

/* Decode one SOS scan into planes (baseline) or coefficients (progressive). */
static int jpeg_scan(struct jpeg *j, unsigned int ns, const unsigned int *sc,
		     int Ss, int Se, int Ah, int Al)
{
	unsigned int k, mcu = 0, total;
	int eobrun = 0;
	int interleaved = (ns > 1) || !j->progressive;

	for (k = 0; k < ns; k++)
		j->comp[sc[k]].dc = 0;
	j->rst_next = 0;

	if (interleaved) {
		unsigned int mx, my;

		total = j->mcus_x * j->mcus_y;
		for (my = 0; my < j->mcus_y; my++) {
			for (mx = 0; mx < j->mcus_x; mx++) {
				for (k = 0; k < ns; k++) {
					struct jpeg_comp *c = &j->comp[sc[k]];
					unsigned int by, bx;

					for (by = 0; by < c->v; by++) {
						for (bx = 0; bx < c->h; bx++) {
							unsigned int index =
								(my * c->v + by) * c->bw +
								(mx * c->h + bx);
							if (j->progressive) {
								if (jpeg_prog_block(j, c,
								    c->coef + index * 64,
								    Ss, Se, Ah, Al,
								    &eobrun))
									return -1;
							} else {
								int blk[64];
								unsigned char p[64];
								unsigned int xx, yy;

								if (jpeg_block(j, c, blk))
									return -1;
								idct_block(blk, p);
								for (yy = 0; yy < 8; yy++)
									for (xx = 0; xx < 8; xx++) {
										unsigned int px = mx * c->h * 8 + bx * 8 + xx;
										unsigned int py = my * c->v * 8 + by * 8 + yy;

										c->plane[py * c->pw + px] =
											p[yy * 8 + xx];
									}
							}
						}
					}
				}
				mcu++;
				if (j->restart && (mcu % j->restart) == 0 &&
				    mcu < total) {
					for (k = 0; k < ns; k++)
						j->comp[sc[k]].dc = 0;
					eobrun = 0;
					if (jpeg_restart(j))
						return -1;
				}
			}
		}
	} else {
		struct jpeg_comp *c = &j->comp[sc[0]];
		unsigned int bx, by;

		total = c->sw * c->sh;
		for (by = 0; by < c->sh; by++) {
			for (bx = 0; bx < c->sw; bx++) {
				unsigned int index = by * c->bw + bx;

				if (jpeg_prog_block(j, c, c->coef + index * 64,
						    Ss, Se, Ah, Al, &eobrun))
					return -1;
				mcu++;
				if (j->restart && (mcu % j->restart) == 0 &&
				    mcu < total) {
					c->dc = 0;
					eobrun = 0;
					if (jpeg_restart(j))
						return -1;
				}
			}
		}
	}
	return 0;
}

static int jpeg_sof(struct jpeg *j, unsigned int marker, int *error)
{
	unsigned int len, prec, i;

	*error = IMG_ERR_CORRUPT;
	if (jpeg_u16(j, &len) || len < 8)
		return -1;
	if (jpeg_u8(j, &prec) || prec != 8) {
		*error = IMG_ERR_UNSUPPORTED;
		return -1;
	}
	if (jpeg_u16(j, &j->h) || jpeg_u16(j, &j->w))
		return -1;
	if (j->w == 0 || j->h == 0 || j->w > 32768 || j->h > 32768) {
		*error = IMG_ERR_TOOBIG;
		return -1;
	}
	if (jpeg_u8(j, &j->ncomp) ||
	    (j->ncomp != 1 && j->ncomp != 3)) {
		*error = IMG_ERR_UNSUPPORTED;
		return -1;
	}
	if (len < 8 + j->ncomp * 3)
		return -1;
	if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) {
		*error = IMG_ERR_UNSUPPORTED;
		return -1;
	}
	j->progressive = (marker == 0xC2);
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
		    j->comp[i].h > 4 || j->comp[i].v > 4 || tq > 3) {
			*error = IMG_ERR_UNSUPPORTED;
			return -1;
		}
		if (j->comp[i].h > j->max_h)
			j->max_h = j->comp[i].h;
		if (j->comp[i].v > j->max_v)
			j->max_v = j->comp[i].v;
	}
	j->mcus_x = (j->w + 8 * j->max_h - 1) / (8 * j->max_h);
	j->mcus_y = (j->h + 8 * j->max_v - 1) / (8 * j->max_v);
	for (i = 0; i < j->ncomp; i++) {
		j->comp[i].bw = j->mcus_x * j->comp[i].h;
		j->comp[i].bh = j->mcus_y * j->comp[i].v;
		j->comp[i].sw = (j->w * j->comp[i].h + 8 * j->max_h - 1) /
				(8 * j->max_h);
		j->comp[i].sh = (j->h * j->comp[i].v + 8 * j->max_v - 1) /
				(8 * j->max_v);
		j->comp[i].pw = j->comp[i].bw * 8;
		j->comp[i].ph = j->comp[i].bh * 8;
	}
	j->out_w = j->w;
	j->out_h = j->h;
	if (j->orientation >= 5) {
		j->out_w = j->h;
		j->out_h = j->w;
	}
	return 0;
}

static int jpeg_setup(struct jpeg *j, struct img_mem *m,
		      struct img_image *out, unsigned int *image_end)
{
	unsigned int i;
	int r;

	r = img_alloc(m, j->out_w, j->out_h, IMG_FMT_JPEG, 0, out);
	if (r)
		return r;
	*image_end = img_mem_mark(m);
	for (i = 0; i < j->ncomp; i++) {
		struct jpeg_comp *c = &j->comp[i];
		unsigned int n;

		c->plane = (unsigned char *)img_mem_alloc(m, c->pw * c->ph, 16);
		if (!c->plane)
			return IMG_ERR_NOMEM;
		if (j->progressive) {
			n = c->bw * c->bh * 64;
			c->coef = (short *)img_mem_alloc(m, n * sizeof(short), 16);
			if (!c->coef)
				return IMG_ERR_NOMEM;
			for (n = 0; n < c->bw * c->bh * 64; n++)
				c->coef[n] = 0;
		}
		c->dc = 0;
	}
	return 0;
}

static void jpeg_reconstruct(struct jpeg *j)
{
	unsigned int ci, bx, by, k;

	for (ci = 0; ci < j->ncomp; ci++) {
		struct jpeg_comp *c = &j->comp[ci];
		const unsigned char *q = j->qt[c->tq];

		for (by = 0; by < c->bh; by++) {
			for (bx = 0; bx < c->bw; bx++) {
				short *coef = c->coef + (by * c->bw + bx) * 64;
				int blk[64];
				unsigned char p[64];
				unsigned int xx, yy;

				for (k = 0; k < 64; k++)
					blk[zigzag[k]] = (int)coef[k] * q[k];
				idct_block(blk, p);
				for (yy = 0; yy < 8; yy++)
					for (xx = 0; xx < 8; xx++)
						c->plane[(by * 8 + yy) * c->pw +
							 bx * 8 + xx] =
							p[yy * 8 + xx];
			}
		}
	}
}

static void jpeg_map_pixel(unsigned int o, unsigned int u, unsigned int v,
			   unsigned int rw, unsigned int rh,
			   unsigned int *x, unsigned int *y)
{
	switch (o) {
	case 2: *x = rw - 1 - u; *y = v; break;
	case 3: *x = rw - 1 - u; *y = rh - 1 - v; break;
	case 4: *x = u; *y = rh - 1 - v; break;
	case 5: *x = v; *y = u; break;
	case 6: *x = v; *y = rh - 1 - u; break;
	case 7: *x = rw - 1 - v; *y = rh - 1 - u; break;
	case 8: *x = rw - 1 - v; *y = u; break;
	default: *x = u; *y = v; break;
	}
}

static void jpeg_output(struct jpeg *j, struct img_image *out)
{
	unsigned int oy, ox;

	for (oy = 0; oy < j->out_h; oy++) {
		for (ox = 0; ox < j->out_w; ox++) {
			unsigned int x, y, yy, r, g, b;

			jpeg_map_pixel(j->orientation, ox, oy, j->w, j->h,
				       &x, &y);
			yy = j->comp[0].plane[y * j->comp[0].pw + x];
			if (j->ncomp == 1) {
				r = g = b = yy;
			} else {
				struct jpeg_comp *cb = &j->comp[1];
				struct jpeg_comp *cr = &j->comp[2];
				unsigned int cstep_x, cstep_y, crstep_x, crstep_y;
				int cpos_x, cpos_y, crpos_x, crpos_y;
				int cbv, crv, yv;
				int rr, gg, bb;

				cstep_x = ((unsigned int)cb->h << 16) / j->max_h;
				cstep_y = ((unsigned int)cb->v << 16) / j->max_v;
				crstep_x = ((unsigned int)cr->h << 16) / j->max_h;
				crstep_y = ((unsigned int)cr->v << 16) / j->max_v;
				cpos_x = (int)(x * cstep_x) +
					(int)(((unsigned int)cb->h * 32768u) / j->max_h) -
					32768;
				cpos_y = (int)(y * cstep_y) +
					(int)(((unsigned int)cb->v * 32768u) / j->max_v) -
					32768;
				crpos_x = (int)(x * crstep_x) +
					(int)(((unsigned int)cr->h * 32768u) / j->max_h) -
					32768;
				crpos_y = (int)(y * crstep_y) +
					(int)(((unsigned int)cr->v * 32768u) / j->max_v) -
					32768;
				cbv = jpeg_chroma(cb, cpos_x, cpos_y) - 128;
				crv = jpeg_chroma(cr, crpos_x, crpos_y) - 128;
				yv = (int)yy;
				rr = yv + ((91881 * crv) >> 16);
				gg = yv - ((22554 * cbv + 46802 * crv) >> 16);
				bb = yv + ((116130 * cbv) >> 16);
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
			out->px[oy * j->out_w + ox] = img_rgb565(r, g, b);
		}
	}
}

/* EXIF orientation tag (IFD0, tag 0x0112); 1 = normal. */
static unsigned int jpeg_exif_orientation(const unsigned char *d, unsigned int n)
{
	unsigned int pos = 2;

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
			if (m == 0xE1 && len >= 8 && pos + 8 <= n &&
			    d[pos + 2] == 'E' && d[pos + 3] == 'x' &&
			    d[pos + 4] == 'i' && d[pos + 5] == 'f' &&
			    d[pos + 6] == 0 && d[pos + 7] == 0) {
				const unsigned char *t = d + pos + 8;
				unsigned int avail = len - 8;
				int le;
				unsigned int magic, ifd;

				if (avail < 8)
					return 1;
				le = (t[0] == 'I' && t[1] == 'I');
				magic = le ? (t[2] | (t[3] << 8)) :
					     ((t[2] << 8) | t[3]);
				if (magic != 42)
					return 1;
				ifd = le ? img_le32(t + 4) : img_be32(t + 4);
				if (ifd + 2 > avail)
					return 1;
				{
					const unsigned char *p = t + ifd;
					unsigned int cnt = le ? img_le16(p) :
							      img_be16(p);
					unsigned int i;

					p += 2;
					for (i = 0; i < cnt; i++) {
						unsigned int tag, type, count, v;

						if ((unsigned int)(p - t) + 12 > avail)
							break;
						tag = le ? img_le16(p) : img_be16(p);
						type = le ? img_le16(p + 2) :
							    img_be16(p + 2);
						count = le ? img_le32(p + 4) :
							     img_be32(p + 4);
						if (tag == 0x0112 && type == 3 &&
						    count == 1) {
							v = le ? img_le16(p + 8) :
								 img_be16(p + 8);
							if (v >= 1 && v <= 8)
								return v;
						}
						p += 12;
					}
				}
				return 1;
			}
			pos += len;
		}
	}
	return 1;
}

int img_sniff_jpeg(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int pos = 2;
	unsigned int orientation = jpeg_exif_orientation(d, n);

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
				unsigned int h = img_be16(d + pos + 3);
				unsigned int w = img_be16(d + pos + 5);
				unsigned int nc = d[pos + 7];
				unsigned int mh = 1, mv = 1, ci;

				if (nc <= 4 && pos + 8 + 3 * nc <= pos + len) {
					for (ci = 0; ci < nc; ci++) {
						unsigned int hv = d[pos + 9 + ci * 3];
						if ((hv >> 4) > mh) mh = hv >> 4;
						if ((hv & 15) > mv) mv = hv & 15;
					}
					out->components = nc;
					out->max_h = mh ? mh : 1;
					out->max_v = mv ? mv : 1;
				}
				out->format = IMG_FMT_JPEG;
				out->height = (orientation >= 5) ? w : h;
				out->width = (orientation >= 5) ? h : w;
				out->has_alpha = 0;
				out->progressive = (m == 0xC2);
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
	unsigned int mark, image_end = 0, i;
	int rcode, setup = 0, scans = 0;

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
	j->orientation = jpeg_exif_orientation(d, n);

	while (j->pos < n) {
		int mk = jpeg_marker(j);

		if (mk < 0)
			goto bad;
		if (mk == 0xD9)
			break;
		if (mk == 0xDA) {
			unsigned int len, ns, ss, se, ahal, k;
			unsigned int sc[3], td[3], ta[3];
			unsigned int ah, al;

			if (jpeg_u16(j, &len) || len < 6)
				goto bad;
			if (jpeg_u8(j, &ns) || ns < 1 || ns > j->ncomp)
				goto bad;
			if (len < 6 + 2 * ns)
				goto bad;
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
				sc[k] = c;
				td[k] = tabs >> 4;
				ta[k] = tabs & 15;
			}
			if (jpeg_u8(j, &ss) || jpeg_u8(j, &se) ||
			    jpeg_u8(j, &ahal))
				goto bad;
			ah = ahal >> 4;
			al = ahal & 15;
			if (len > 6 + 2 * ns)
				j->pos += len - (6 + 2 * ns);
			if (!j->progressive) {
				if (ns != j->ncomp || ss != 0 || se != 63 ||
				    ah != 0 || al != 0)
					goto unsupported;
			} else {
				if (ss == 0) {
					if (se != 0)
						goto bad;
				} else if (ns != 1 || ss > se || se > 63) {
					goto bad;
				}
				if (ah > 13 || al > 13)
					goto bad;
			}
			if (!setup)
				goto bad;
			for (k = 0; k < ns; k++) {
				j->comp[sc[k]].td = (int)td[k];
				j->comp[sc[k]].ta = (int)ta[k];
			}
			if (jpeg_scan(j, ns, sc, (int)ss, (int)se,
				      (int)ah, (int)al))
				goto entropy_bad;
			scans++;
			if (!j->progressive)
				break;
			jpeg_scan_end(j);
			continue;
		}
		if (mk == 0xC0 || mk == 0xC1 || mk == 0xC2) {
			int err = IMG_ERR_CORRUPT;

			if (setup)
				goto bad;
			if (jpeg_sof(j, (unsigned int)mk, &err)) {
				rcode = err;
				goto fail;
			}
			if (!img_size_ok(j->w, j->h, opt))
				goto too_big;
			rcode = jpeg_setup(j, m, out, &image_end);
			if (rcode)
				goto fail;
			setup = 1;
			continue;
		}
		if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 &&
		    mk != 0xC8 && mk != 0xCC) {
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
	if (!setup || scans == 0)
		goto bad;
	if (j->progressive)
		jpeg_reconstruct(j);
	jpeg_output(j, out);
	img_mem_rewind(m, image_end);
	return 0;

entropy_bad:
	img_mem_rewind(m, mark);
	rcode = IMG_ERR_CORRUPT;
	goto fail;
too_big:
	rcode = IMG_ERR_TOOBIG;
	goto fail;
unsupported:
	rcode = IMG_ERR_UNSUPPORTED;
	goto fail;
bad:
	rcode = IMG_ERR_CORRUPT;
fail:
	img_mem_rewind(m, mark);
	return rcode;
}
