/* PAM (P7): binary grayscale/RGB with optional alpha. */

#include "img_internal.h"

struct pam_hdr {
	unsigned int w, h, depth, maxval, data;
};

static int pam_line(const unsigned char *d, unsigned int n, unsigned int *pos,
		    const unsigned char **start, unsigned int *len)
{
	unsigned int p = *pos, s;

	if (p >= n)
		return -1;
	s = p;
	while (p < n && d[p] != '\n')
		p++;
	*start = d + s;
	*len = p - s;
	if (p < n)
		p++;
	*pos = p;
	if (*len && (*start)[*len - 1] == '\r')
		(*len)--;
	return 0;
}

static int pam_uint(const unsigned char *s, unsigned int len, unsigned int *v)
{
	unsigned int i = 0, val = 0;
	int any = 0;

	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') {
		val = val * 10 + (s[i] - '0');
		if (val > 65535)
			return -1;
		i++;
		any = 1;
	}
	if (!any)
		return -1;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i != len)
		return -1;
	*v = val;
	return 0;
}

static int pam_key(const unsigned char *s, unsigned int len, const char *key)
{
	unsigned int i = 0, k = 0;

	while (i < len && s[i] == ' ')
		i++;
	while (key[k]) {
		if (i >= len || s[i] != (unsigned char)key[k])
			return 0;
		i++;
		k++;
	}
	return i == len || s[i] == ' ' || s[i] == '\t';
}


static int pam_header(const unsigned char *d, unsigned int n, struct pam_hdr *p)
{
	unsigned int pos = 2;
	int done = 0;

	p->w = p->h = p->depth = p->maxval = 0;
	while (pos < n) {
		const unsigned char *s;
		unsigned int len, v;

		if (pam_line(d, n, &pos, &s, &len))
			return -1;
		if (!len || s[0] == '#')
			continue;
		if (len == 6 && s[0] == 'E' && s[1] == 'N' && s[2] == 'D' &&
		    s[3] == 'H' && s[4] == 'D' && s[5] == 'R') {
			done = 1;
			break;
		}
		if (pam_key(s, len, "WIDTH")) {
			if (pam_uint(s + 5, len - 5, &v))
				return -1;
			p->w = v;
		} else if (pam_key(s, len, "HEIGHT")) {
			if (pam_uint(s + 6, len - 6, &v))
				return -1;
			p->h = v;
		} else if (pam_key(s, len, "DEPTH")) {
			if (pam_uint(s + 5, len - 5, &v))
				return -1;
			p->depth = v;
		} else if (pam_key(s, len, "MAXVAL")) {
			if (pam_uint(s + 6, len - 6, &v))
				return -1;
			p->maxval = v;
		} else if (pam_key(s, len, "TUPLTYPE")) {
			/* The channel count from DEPTH is sufficient. */
		} else {
			return -1;
		}
	}
	if (!done || p->w == 0 || p->h == 0 || p->w > 32768 ||
	    p->h > 32768 || p->depth < 1 || p->depth > 4 ||
	    p->maxval == 0 || p->maxval > 65535)
		return -1;
	p->data = pos;
	return 0;
}

int img_sniff_pam(const unsigned char *d, unsigned int n, struct img_info *out)
{
	struct pam_hdr p;

	if (pam_header(d, n, &p))
		return IMG_FMT_UNKNOWN;
	out->format = IMG_FMT_PAM;
	out->width = p.w;
	out->height = p.h;
	out->has_alpha = (p.depth == 2 || p.depth == 4);
	out->progressive = 0;
	return IMG_FMT_PAM;
}

static unsigned int pam_scale(unsigned int v, unsigned int mv)
{
	if (mv == 255)
		return v;
	if (mv == 1)
		return v ? 255 : 0;
	return (v * 255u + mv / 2) / mv;
}

int img_decode_pam(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out)
{
	struct pam_hdr p;
	unsigned int x, y, c;
	unsigned int step;
	int alpha, rcode;

	if (pam_header(d, n, &p))
		return IMG_ERR_FORMAT;
	if (!img_size_ok(p.w, p.h, opt))
		return IMG_ERR_TOOBIG;
	alpha = (p.depth == 2 || p.depth == 4);
	rcode = img_alloc(m, p.w, p.h, IMG_FMT_PAM, alpha, out);
	if (rcode)
		return rcode;
	step = p.maxval > 255 ? 2 : 1;
	for (y = 0; y < p.h; y++) {
		for (x = 0; x < p.w; x++) {
			unsigned int s[4] = { 0, 0, 0, 0 };
			unsigned int r, g, b, a = 255;

			for (c = 0; c < p.depth; c++) {
				if (p.data + step > n)
					return IMG_ERR_CORRUPT;
				if (step == 1) {
					s[c] = d[p.data++];
				} else {
					s[c] = img_be16(d + p.data);
					p.data += 2;
				}
				if (s[c] > p.maxval)
					return IMG_ERR_CORRUPT;
				s[c] = pam_scale(s[c], p.maxval);
			}
			if (p.depth <= 2) {
				r = g = b = s[0];
				if (p.depth == 2)
					a = s[1];
			} else {
				r = s[0];
				g = s[1];
				b = s[2];
				if (p.depth == 4)
					a = s[3];
			}
			out->px[y * p.w + x] = img_rgb565(r, g, b);
			if (out->alpha)
				out->alpha[y * p.w + x] = (unsigned char)a;
		}
	}
	return 0;
}
