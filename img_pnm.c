/* PNM: P1/P2/P3 (ASCII) and P4/P5/P6 (binary), 8- or 16-bit samples. */

#include "img_internal.h"

struct pnm_reader {
	const unsigned char *d;
	unsigned int n, pos;
};

static int pnm_ws(struct pnm_reader *r)
{
	for (;;) {
		while (r->pos < r->n) {
			unsigned char c = r->d[r->pos];

			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
			    c == '\f' || c == '\v') {
				r->pos++;
			} else {
				break;
			}
		}
		if (r->pos < r->n && r->d[r->pos] == '#') {
			while (r->pos < r->n && r->d[r->pos] != '\n')
				r->pos++;
			continue;
		}
		return r->pos < r->n ? 0 : -1;
	}
}

static int pnm_uint(struct pnm_reader *r, unsigned int *out)
{
	unsigned int v = 0;
	int any = 0;

	if (pnm_ws(r))
		return -1;
	while (r->pos < r->n) {
		unsigned char c = r->d[r->pos];

		if (c < '0' || c > '9')
			break;
		v = v * 10 + (c - '0');
		if (v > 0x7FFFFFFF)
			return -1;
		r->pos++;
		any = 1;
	}
	if (!any)
		return -1;
	*out = v;
	return 0;
}

static int pnm_header(const unsigned char *d, unsigned int n,
		      unsigned int *kind, unsigned int *w, unsigned int *h,
		      unsigned int *maxval, unsigned int *hdr_end)
{
	struct pnm_reader r;

	if (n < 2 || d[0] != 'P' || d[1] < '1' || d[1] > '6')
		return -1;
	r.d = d;
	r.n = n;
	r.pos = 2;
	*kind = (unsigned int)(d[1] - '0');
	if (pnm_uint(&r, w) || pnm_uint(&r, h))
		return -1;
	if (*kind == 1 || *kind == 4) {
		*maxval = 1;
	} else if (pnm_uint(&r, maxval) || *maxval == 0 || *maxval > 65535) {
		return -1;
	}
	if (*w == 0 || *h == 0 || *w > 32768 || *h > 32768)
		return -1;
	/* Binary formats have exactly one whitespace byte after the header
	 * (it may not be skipped like ASCII separators: a pixel may itself be
	 * a whitespace value). */
	if (*kind >= 4) {
		unsigned char c;

		if (r.pos >= r.n)
			return -1;
		c = r.d[r.pos];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n' &&
		    c != '\f' && c != '\v')
			return -1;
		r.pos++;
		if (c == '\r' && r.pos < r.n && r.d[r.pos] == '\n')
			r.pos++;       /* tolerate CRLF */
	} else {
		if (pnm_ws(&r))
			return -1;
	}
	*hdr_end = r.pos;
	return 0;
}

int img_sniff_pnm(const unsigned char *d, unsigned int n, struct img_info *out)
{
	unsigned int kind, w, h, mv, end;

	if (pnm_header(d, n, &kind, &w, &h, &mv, &end))
		return IMG_FMT_UNKNOWN;
	out->format = IMG_FMT_PNM;
	out->width = w;
	out->height = h;
	out->has_alpha = 0;
	return IMG_FMT_PNM;
}

static unsigned int pnm_scale(unsigned int v, unsigned int mv)
{
	if (mv == 255)
		return v;
	if (mv == 1)
		return v ? 255 : 0;
	return (v * 255u + mv / 2) / mv;
}

static int pnm_ascii_sample(struct pnm_reader *r, unsigned int mv,
			    unsigned int *out)
{
	unsigned int v;

	if (pnm_uint(r, &v))
		return -1;
	if (v > mv)
		return -1;
	*out = pnm_scale(v, mv);
	return 0;
}

int img_decode_pnm(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	unsigned int kind, w, h, mv, end;
	struct pnm_reader r;
	unsigned int x, y;
	unsigned int maxv;
	int rcode;

	if (pnm_header(d, n, &kind, &w, &h, &mv, &end))
		return IMG_ERR_FORMAT;
	if (!img_size_ok(w, h, opt))
		return IMG_ERR_TOOBIG;
	r.d = d;
	r.n = n;
	r.pos = end;
	maxv = mv;

	rcode = img_alloc(m, w, h, IMG_FMT_PNM, 0, out);
	if (rcode)
		return rcode;

	if (kind == 4) {
		unsigned int stride = (w + 7) / 8;

		if (end > n || (unsigned long long)stride * h > n - end)
			return IMG_ERR_CORRUPT;
		for (y = 0; y < h; y++) {
			const unsigned char *row = d + end + y * stride;

			for (x = 0; x < w; x++) {
				unsigned int bit = (row[x / 8] >> (7 - (x & 7))) & 1;
				out->px[y * w + x] = bit ? 0 : 0xFFFF;
			}
		}
		return 0;
	}

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned int rr, gg, bb;

			if (kind == 1) {
				if (pnm_ascii_sample(&r, 1, &rr))
					return IMG_ERR_CORRUPT;
				gg = bb = rr;
			} else if (kind == 2 || kind == 5) {
				if (kind == 2) {
					if (pnm_ascii_sample(&r, maxv, &rr))
						return IMG_ERR_CORRUPT;
				} else {
					if (maxv < 256) {
						if (r.pos >= n)
							return IMG_ERR_CORRUPT;
						rr = pnm_scale(d[r.pos++], maxv);
					} else {
						if (r.pos + 2 > n)
							return IMG_ERR_CORRUPT;
						rr = pnm_scale(img_be16(d + r.pos), maxv);
						r.pos += 2;
					}
				}
				gg = bb = rr;
			} else {
				if (kind == 3) {
					if (pnm_ascii_sample(&r, maxv, &rr) ||
					    pnm_ascii_sample(&r, maxv, &gg) ||
					    pnm_ascii_sample(&r, maxv, &bb))
						return IMG_ERR_CORRUPT;
				} else {
					unsigned int c[3], k;

					for (k = 0; k < 3; k++) {
						if (maxv < 256) {
							if (r.pos >= n)
								return IMG_ERR_CORRUPT;
							c[k] = pnm_scale(d[r.pos++], maxv);
						} else {
							if (r.pos + 2 > n)
								return IMG_ERR_CORRUPT;
							c[k] = pnm_scale(img_be16(d + r.pos), maxv);
							r.pos += 2;
						}
					}
					rr = c[0];
					gg = c[1];
					bb = c[2];
				}
			}
			out->px[y * w + x] = img_rgb565(rr, gg, bb);
		}
	}
	return 0;
}
