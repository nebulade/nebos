/* Minimal fixed-point TrueType parser and rasterizer.
 *
 * No floats, no hinting, no CFF.  Supports the tables and glyph forms that the
 * Latin-1 Arimo/Liberation subsets use: simple and composite glyphs, cmap
 * format 4, short/long loca, hmtx advances.  Coordinates are scaled to 26.6
 * pixels; coverage is 4x vertical supersampling with exact horizontal spans.
 */
#include "ufont_internal.h"
#include <string.h>

#define TTF_MAX_EDGES 2048

struct ttf_point {
	int x, y;
	unsigned char on;
};

struct ttf_outline {
	int npoints;
	int ncontours;
	unsigned short ends[UFONT_MAX_CONTOURS];
	struct ttf_point points[UFONT_MAX_POINTS];
};

struct ttf_seq_point {
	int x, y;
	unsigned char on;
};

struct ttf_edge {
	int x0, y0, x1, y1;
	signed char dir;
};

static struct ttf_outline comp_outline[4];
static struct ttf_seq_point seq_points[UFONT_MAX_POINTS * 2];
static struct ttf_edge edges[TTF_MAX_EDGES];
static int n_edges;

static unsigned int be16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | p[1];
}

static int sbe16(const unsigned char *p)
{
	return (int)(short)be16(p);
}

static unsigned int be32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static int tag_eq(const unsigned char *p, const char *tag)
{
	return p[0] == (unsigned char)tag[0] &&
	       p[1] == (unsigned char)tag[1] &&
	       p[2] == (unsigned char)tag[2] &&
	       p[3] == (unsigned char)tag[3];
}

static const unsigned char *find_table(const unsigned char *data,
				       unsigned int len, const char *tag,
				       unsigned int *out_len)
{
	unsigned int n, i;
	const unsigned char *dir;

	if (len < 12)
		return 0;
	n = be16(data + 4);
	dir = data + 12;
	if (n > 64 || 12u + n * 16u > len)
		return 0;
	for (i = 0; i < n; i++) {
		const unsigned char *r = dir + i * 16;
		unsigned int off = be32(r + 8);
		unsigned int l = be32(r + 12);

		if (!tag_eq(r, tag))
			continue;
		if (off > len || l > len - off)
			return 0;
		if (out_len)
			*out_len = l;
		return data + off;
	}
	return 0;
}

int ufont_ttf_load(struct ufont_face *f, const unsigned char *data,
		   unsigned int len)
{
	const unsigned char *head, *maxp, *hhea, *hmtx, *loca, *glyf, *cmap;
	unsigned int head_len, maxp_len, hhea_len, hmtx_len, loca_len;
	unsigned int glyf_len, cmap_len, i, nrec;
	unsigned int cmap4_off = 0;

	if (len < 12)
		return -1;
	/* 0x00010000 is TrueType; "true" also appears in older files. */
	if (!((data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0) ||
	      (data[0] == 't' && data[1] == 'r' && data[2] == 'u' &&
	       data[3] == 'e')))
		return -1;
	head = find_table(data, len, "head", &head_len);
	maxp = find_table(data, len, "maxp", &maxp_len);
	hhea = find_table(data, len, "hhea", &hhea_len);
	hmtx = find_table(data, len, "hmtx", &hmtx_len);
	loca = find_table(data, len, "loca", &loca_len);
	glyf = find_table(data, len, "glyf", &glyf_len);
	cmap = find_table(data, len, "cmap", &cmap_len);
	if (!head || !maxp || !hhea || !hmtx || !loca || !glyf || !cmap)
		return -1;
	if (head_len < 54 || maxp_len < 6 || hhea_len < 36)
		return -1;
	if (be32(head + 12) != 0x5F0F3CF5u)   /* head.magicNumber */
		return -1;

	f->data = data;
	f->len = len;
	f->upem = be16(head + 18);
	f->loca_short = sbe16(head + 50) == 0;
	f->num_glyphs = be16(maxp + 4);
	f->ascent = sbe16(hhea + 4);
	f->descent = sbe16(hhea + 6);
	f->line_gap = sbe16(hhea + 8);
	f->num_hmetrics = be16(hhea + 34);
	if (f->upem < 16 || f->upem > 16384 || f->num_glyphs < 1 ||
	    f->num_glyphs > 65535)
		return -1;
	if (f->num_hmetrics < 1 || f->num_hmetrics > f->num_glyphs)
		return -1;
	if (loca_len < (f->loca_short ? (unsigned int)f->num_glyphs * 2u + 2u
				      : (unsigned int)f->num_glyphs * 4u + 4u))
		return -1;
	if (hmtx_len < (unsigned int)f->num_hmetrics * 4u)
		return -1;

	/* Pick a format 4 Unicode cmap. */
	if (cmap_len < 4)
		return -1;
	nrec = be16(cmap + 2);
	if (nrec > 64 || 4u + nrec * 8u > cmap_len)
		return -1;
	for (i = 0; i < nrec; i++) {
		const unsigned char *r = cmap + 4 + i * 8;
		unsigned int off = be32(r + 4);
		unsigned int plat = be16(r);
		unsigned int enc = be16(r + 2);

		if (off + 2 > cmap_len)
			continue;
		if (be16(cmap + off) != 4)
			continue;
		if (plat == 3 && (enc == 0 || enc == 1)) {
			cmap4_off = off;
			break;
		}
		if (plat == 0 && enc == 3 && !cmap4_off)
			cmap4_off = off;
	}
	if (!cmap4_off || cmap4_off + 16 > cmap_len)
		return -1;
	if (be16(cmap + cmap4_off + 2) > cmap_len - cmap4_off)
		return -1;

	f->loca = loca;
	f->glyf = glyf;
	f->glyf_len = glyf_len;
	f->hmtx = hmtx;
	f->cmap4 = cmap + cmap4_off;
	f->cmap4_len = be16(cmap + cmap4_off + 2);
	return 0;
}

int ufont_ttf_glyph_index(const struct ufont_face *f, unsigned int cp)
{
	const unsigned char *c = f->cmap4;
	unsigned int seg, i, end, start, delta, range;

	if (cp > 0xFFFFu || !c || f->cmap4_len < 16)
		return 0;
	seg = be16(c + 6) / 2u;
	if (seg == 0 || 16u + seg * 8u > f->cmap4_len)
		return 0;
	for (i = 0; i < seg; i++) {
		end = be16(c + 14 + i * 2);
		if (cp > end)
			continue;
		start = be16(c + 16 + seg * 2 + i * 2);
		if (cp < start)
			return 0;
		delta = be16(c + 16 + seg * 4 + i * 2);
		range = be16(c + 16 + seg * 6 + i * 2);
		if (range == 0)
			return (int)((cp + delta) & 0xFFFFu);
		{
			unsigned int ro = 16u + seg * 6u + i * 2u;
			unsigned int addr = ro + range + (cp - start) * 2u;
			unsigned int g;

			if (addr + 2u > f->cmap4_len)
				return 0;
			g = be16(c + addr);
			if (g)
				g = (g + delta) & 0xFFFFu;
			return (int)g;
		}
	}
	return 0;
}

static const unsigned char *glyph_data(const struct ufont_face *f, int glyph,
				       unsigned int *size)
{
	unsigned int a, b;

	if (glyph < 0 || glyph >= f->num_glyphs)
		return 0;
	if (f->loca_short) {
		a = be16(f->loca + glyph * 2u) * 2u;
		b = be16(f->loca + glyph * 2u + 2u) * 2u;
	} else {
		a = be32(f->loca + glyph * 4u);
		b = be32(f->loca + glyph * 4u + 4u);
	}
	if (b <= a || b > f->glyf_len)
		return 0;
	if (size)
		*size = b - a;
	return f->glyf + a;
}

int ufont_ttf_advance(const struct ufont_face *f, int glyph, int px)
{
	unsigned int units;

	if (glyph < 0 || glyph >= f->num_glyphs)
		glyph = 0;
	if (glyph >= f->num_hmetrics)
		glyph = f->num_hmetrics - 1;
	units = be16(f->hmtx + glyph * 4u);
	return (int)((units * (unsigned int)px * 64u + (unsigned int)f->upem / 2u)
		     / (unsigned int)f->upem);
}

static int load_simple(const struct ufont_face *f, const unsigned char *g,
		       unsigned int glen, struct ttf_outline *out)
{
	int ncontours = sbe16(g);
	(void)f;
	int i, j, npoints, flags, repeats;
	unsigned int off;
	unsigned char flag;
	int x, y;

	if (ncontours < 0 || ncontours > UFONT_MAX_CONTOURS)
		return -1;
	if (glen < 12u + (unsigned int)ncontours * 2u)
		return -1;
	out->ncontours = ncontours;
	for (i = 0; i < ncontours; i++)
		out->ends[i] = be16(g + 10 + i * 2);
	npoints = ncontours ? (int)out->ends[ncontours - 1] + 1 : 0;
	if (npoints < 0 || npoints > UFONT_MAX_POINTS)
		return -1;
	off = 10u + (unsigned int)ncontours * 2u;
	if (off + 2u > glen)
		return -1;
	off += 2u + be16(g + off);     /* skip instructions */
	if (off > glen)
		return -1;

	/* Flags, with the repeat bit expanded. */
	{
		unsigned char flags_buf[UFONT_MAX_POINTS];
		int n = 0;

		while (n < npoints) {
			if (off >= glen)
				return -1;
			flag = g[off++];
			flags_buf[n++] = flag;
			if (flag & 0x08) {
				if (off >= glen)
					return -1;
				repeats = g[off++];
				while (repeats-- > 0 && n < npoints)
					flags_buf[n++] = flag;
			}
		}
		for (i = 0; i < npoints; i++) {
			flags = flags_buf[i];
			out->points[i].on = (flags & 0x01) != 0;
			out->points[i].x = 0;
			out->points[i].y = 0;
		}
		/* X deltas. */
		x = 0;
		for (i = 0; i < npoints; i++) {
			flags = flags_buf[i];
			if (flags & 0x02) {
				if (off >= glen)
					return -1;
				j = g[off++];
				x += (flags & 0x10) ? j : -j;
			} else if (!(flags & 0x10)) {
				if (off + 2u > glen)
					return -1;
				x += sbe16(g + off);
				off += 2;
			}
			out->points[i].x = x;
		}
		/* Y deltas. */
		y = 0;
		for (i = 0; i < npoints; i++) {
			flags = flags_buf[i];
			if (flags & 0x04) {
				if (off >= glen)
					return -1;
				j = g[off++];
				y += (flags & 0x20) ? j : -j;
			} else if (!(flags & 0x20)) {
				if (off + 2u > glen)
					return -1;
				y += sbe16(g + off);
				off += 2;
			}
			out->points[i].y = y;
		}
	}
	out->npoints = npoints;
	return 0;
}

static int load_glyph(const struct ufont_face *f, int glyph,
		      struct ttf_outline *out, int depth);

static int load_composite(const struct ufont_face *f, const unsigned char *g,
			  unsigned int glen, struct ttf_outline *out, int depth)
{
	unsigned int off = 10;
	int any = 0;

	out->npoints = 0;
	out->ncontours = 0;
	if (depth >= 4)
		return -1;
	for (;;) {
		unsigned int flags, comp, args[2];
		int dx, dy, i, a, b, c, d;
		struct ttf_outline *co;

		if (off + 4u > glen)
			return -1;
		flags = be16(g + off);
		comp = be16(g + off + 2);
		off += 4;
		if (flags & 0x0001) {
			if (off + 4u > glen)
				return -1;
			args[0] = (unsigned int)sbe16(g + off);
			args[1] = (unsigned int)sbe16(g + off + 2);
			off += 4;
		} else {
			if (off + 2u > glen)
				return -1;
			args[0] = (unsigned int)(signed char)g[off];
			args[1] = (unsigned int)(signed char)g[off + 1];
			off += 2;
		}
		a = 0x10000; b = 0; c = 0; d = 0x10000;
		if (flags & 0x0008) {
			if (off + 2u > glen)
				return -1;
			a = d = sbe16(g + off) * 4;       /* F2Dot14 -> 16.16 */
			off += 2;
		} else if (flags & 0x0040) {
			if (off + 4u > glen)
				return -1;
			a = sbe16(g + off) * 4;
			d = sbe16(g + off + 2) * 4;
			off += 4;
		} else if (flags & 0x0080) {
			if (off + 8u > glen)
				return -1;
			a = sbe16(g + off) * 4;
			b = sbe16(g + off + 2) * 4;
			c = sbe16(g + off + 4) * 4;
			d = sbe16(g + off + 6) * 4;
			off += 8;
		}
		if (comp >= (unsigned int)f->num_glyphs ||
		    depth + 1 >= 4)
			return -1;
		co = &comp_outline[depth + 1];
		if (load_glyph(f, (int)comp, co, depth + 1) != 0)
			return -1;

		/* Apply the component transform in font units. */
		for (i = 0; i < co->npoints; i++) {
			int x = co->points[i].x;
			int y = co->points[i].y;

			co->points[i].x = (a * x + c * y) >> 16;
			co->points[i].y = (b * x + d * y) >> 16;
		}
		dx = 0;
		dy = 0;
		if (flags & 0x0002) {
			dx = (int)args[0];
			dy = (int)args[1];
			if ((flags & 0x0800) && !(flags & 0x1000)) {
				int tx = (a * dx + c * dy) >> 16;
				int ty = (b * dx + d * dy) >> 16;

				dx = tx;
				dy = ty;
			}
		} else {
			int p1 = (int)args[0];
			int p2 = (int)args[1];

			if (p1 >= 0 && p1 < out->npoints &&
			    p2 >= 0 && p2 < co->npoints) {
				dx = out->points[p1].x - co->points[p2].x;
				dy = out->points[p1].y - co->points[p2].y;
			}
		}
		if (out->npoints + co->npoints > UFONT_MAX_POINTS ||
		    out->ncontours + co->ncontours > UFONT_MAX_CONTOURS)
			return -1;
		for (i = 0; i < co->npoints; i++) {
			out->points[out->npoints + i] = co->points[i];
			out->points[out->npoints + i].x += dx;
			out->points[out->npoints + i].y += dy;
		}
		for (i = 0; i < co->ncontours; i++)
			out->ends[out->ncontours + i] =
				(unsigned short)(co->ends[i] + out->npoints);
		out->npoints += co->npoints;
		out->ncontours += co->ncontours;
		any = 1;
		if (!(flags & 0x0020))
			break;
	}
	return any ? 0 : -1;
}

static int load_glyph(const struct ufont_face *f, int glyph,
		      struct ttf_outline *out, int depth)
{
	const unsigned char *g;
	unsigned int glen;
	int ncontours;

	g = glyph_data(f, glyph, &glen);
	if (!g)
		return -1;
	if (glen < 10)
		return -1;
	ncontours = sbe16(g);
	if (ncontours >= 0)
		return load_simple(f, g, glen, out);
	return load_composite(f, g, glen, out, depth);
}

static int scale_coord(const struct ufont_face *f, int v, int px)
{
	return (int)(((long)v * (long)px * 64L +
		      (long)f->upem / 2L) / (long)f->upem);
}

static int floor_div64(int v)
{
	return v >= 0 ? v / 64 : -((-v + 63) / 64);
}

static int ceil_div64(int v)
{
	return v >= 0 ? (v + 63) / 64 : -((-v) / 64);
}

int ufont_ttf_bbox(const struct ufont_face *f, int glyph, int px,
		   int *xoff, int *yoff, int *w, int *h)
{
	static struct ttf_outline out;
	int i, minx, miny, maxx, maxy, x0, y0, x1, y1;

	*xoff = 0;
	*yoff = 0;
	*w = 0;
	*h = 0;
	if (load_glyph(f, glyph, &out, 0) != 0 || out.npoints == 0)
		return 0;
	minx = maxx = scale_coord(f, out.points[0].x, px);
	miny = maxy = -scale_coord(f, out.points[0].y, px);
	for (i = 1; i < out.npoints; i++) {
		int x = scale_coord(f, out.points[i].x, px);
		int y = -scale_coord(f, out.points[i].y, px);

		if (x < minx) minx = x;
		if (x > maxx) maxx = x;
		if (y < miny) miny = y;
		if (y > maxy) maxy = y;
	}
	x0 = floor_div64(minx);
	y0 = floor_div64(miny);
	x1 = ceil_div64(maxx);
	y1 = ceil_div64(maxy);
	if (x1 <= x0) x1 = x0 + 1;
	if (y1 <= y0) y1 = y0 + 1;
	*xoff = x0;
	*yoff = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	if (*w > 512 || *h > 512) {
		*w = *h = 0;
		return 0;
	}
	return 0;
}

static void add_edge(int x0, int y0, int x1, int y1)
{
	signed char dir;

	if (y0 == y1 || n_edges >= TTF_MAX_EDGES)
		return;
	dir = 1;
	if (y0 > y1) {
		int t;
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		dir = -1;
	}
	edges[n_edges].x0 = x0;
	edges[n_edges].y0 = y0;
	edges[n_edges].x1 = x1;
	edges[n_edges].y1 = y1;
	edges[n_edges].dir = dir;
	n_edges++;
}

static void flatten_quad(int x0, int y0, int cx, int cy, int x1, int y1,
			 int depth)
{
	long dx = (long)x1 - x0;
	long dy = (long)y1 - y0;
	long cross = dx * ((long)cy - y0) - dy * ((long)cx - x0);
	long lim = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

	if (lim < 1)
		lim = 1;
	if (cross < 0)
		cross = -cross;
	if (depth >= 8 || cross <= lim * 2L) {
		add_edge(x0, y0, x1, y1);
		return;
	}
	{
		int q0x = (x0 + cx) / 2, q0y = (y0 + cy) / 2;
		int q1x = (cx + x1) / 2, q1y = (cy + y1) / 2;
		int mx = (q0x + q1x) / 2, my = (q0y + q1y) / 2;

		flatten_quad(x0, y0, q0x, q0y, mx, my, depth + 1);
		flatten_quad(mx, my, q1x, q1y, x1, y1, depth + 1);
	}
}

static void build_edges(const struct ufont_face *f, int px,
			const struct ttf_outline *out)
{
	int c, start = 0;

	n_edges = 0;
	for (c = 0; c < out->ncontours; c++) {
		int end = out->ends[c];
		int count = end - start + 1;
		int first_on = -1, i, n = 0;

		if (count <= 1) {
			start = end + 1;
			continue;
		}
		for (i = 0; i < count; i++)
			if (out->points[start + i].on) {
				first_on = i;
				break;
			}
		if (first_on < 0) {
			/* All off-curve: insert a midpoint between every pair
			 * and start at the midpoint before the first point. */
			for (i = 0; i < count; i++) {
				int a = start + i;
				int b = start + ((i + 1) % count);

				if (n < (int)(sizeof(seq_points) /
					      sizeof(seq_points[0]))) {
					seq_points[n].x =
						(out->points[a].x +
						 out->points[b].x) / 2;
					seq_points[n].y =
						(out->points[a].y +
						 out->points[b].y) / 2;
					seq_points[n].on = 1;
					n++;
				}
				if (n < (int)(sizeof(seq_points) /
					      sizeof(seq_points[0]))) {
					seq_points[n] = (struct ttf_seq_point){
						out->points[b].x,
						out->points[b].y,
						out->points[b].on
					};
					n++;
				}
			}
		} else {
			/* Rotate to the first on-curve point and insert
			 * midpoints between consecutive off-curve points. */
			for (i = 0; i < count; i++) {
				int idx = start + (first_on + i) % count;
				struct ttf_seq_point p;

				p.x = out->points[idx].x;
				p.y = out->points[idx].y;
				p.on = out->points[idx].on;
				if (n > 0 && !seq_points[n - 1].on && !p.on) {
					seq_points[n].x =
						(seq_points[n - 1].x + p.x) / 2;
					seq_points[n].y =
						(seq_points[n - 1].y + p.y) / 2;
					seq_points[n].on = 1;
					n++;
				}
				seq_points[n++] = p;
				if (n >= (int)(sizeof(seq_points) /
					       sizeof(seq_points[0])))
					break;
			}
		}

		for (i = 0; i < n; i++) {
			struct ttf_seq_point a = seq_points[i];
			struct ttf_seq_point b = seq_points[(i + 1) % n];
			int ax = scale_coord(f, a.x, px);
			int ay = -scale_coord(f, a.y, px);
			int bx = scale_coord(f, b.x, px);
			int by = -scale_coord(f, b.y, px);

			if (b.on) {
				add_edge(ax, ay, bx, by);
			} else {
				struct ttf_seq_point d =
					seq_points[(i + 2) % n];
				int cx = bx, cy = by;
				int dx = scale_coord(f, d.x, px);
				int dy = -scale_coord(f, d.y, px);

				flatten_quad(ax, ay, cx, cy, dx, dy, 0);
				i++;    /* the next point is consumed */
			}
		}
		start = end + 1;
	}
}

/* Add horizontal coverage of [xa, xb] (26.6, relative to xoff) to mask. */
static void add_span(unsigned char *mask, int w, int h, int row,
		     int xa, int xb)
{
	int pa, pb, p;
	(void)h;

	if (xa < 0)
		xa = 0;
	if (xb > w * 64)
		xb = w * 64;
	if (xb <= xa)
		return;
	pa = xa / 64;
	pb = (xb + 63) / 64;
	if (pa < 0)
		pa = 0;
	if (pb > w)
		pb = w;
	for (p = pa; p < pb; p++) {
		int l = p * 64;
		int r = (p + 1) * 64;
		int cov;
		int v;

		if (l < xa) l = xa;
		if (r > xb) r = xb;
		cov = r - l;
		if (cov <= 0)
			continue;
		v = mask[row * w + p] + cov;
		if (v > 255)
			v = 255;
		mask[row * w + p] = (unsigned char)v;
	}
}

int ufont_ttf_raster(const struct ufont_face *f, int glyph, int px,
		     int xoff, int yoff, int w, int h, unsigned char *mask)
{
	static struct ttf_outline out;
	int row, sub;

	if (w <= 0 || h <= 0 || !mask)
		return 0;
	if (load_glyph(f, glyph, &out, 0) != 0 || out.npoints == 0)
		return 0;
	build_edges(f, px, &out);
	if (n_edges == 0)
		return 0;

	for (row = 0; row < h; row++) {
		for (sub = 0; sub < 4; sub++) {
			int sy = ((yoff + row) * 64) + sub * 16 + 8;
			static int xs[TTF_MAX_EDGES];
			static signed char dirs[TTF_MAX_EDGES];
			int n = 0, i, j, wind = 0, span = 0, span_x = 0;

			for (i = 0; i < n_edges; i++) {
				const struct ttf_edge *e = &edges[i];
				long dy, num, x;

				if (sy < e->y0 || sy >= e->y1)
					continue;
				dy = (long)e->y1 - e->y0;
				if (dy == 0)
					continue;
				num = (long)(sy - e->y0) *
				      ((long)e->x1 - e->x0);
				x = e->x0 + num / dy;
				if (n < TTF_MAX_EDGES) {
					xs[n] = (int)x;
					dirs[n] = e->dir;
					n++;
				}
			}
			/* Insertion sort by x. */
			for (i = 1; i < n; i++) {
				int xv = xs[i];
				signed char dv = dirs[i];

				j = i - 1;
				while (j >= 0 && xs[j] > xv) {
					xs[j + 1] = xs[j];
					dirs[j + 1] = dirs[j];
					j--;
				}
				xs[j + 1] = xv;
				dirs[j + 1] = dv;
			}
			wind = 0;
			span = 0;
			span_x = 0;
			for (i = 0; i < n; i++) {
				if (wind == 0) {
					span_x = xs[i];
					span = 1;
				}
				wind += dirs[i];
				if (wind == 0 && span) {
					int xa = span_x - (xoff * 64);
					int xb = xs[i] - (xoff * 64);

					if (xb > xa)
						add_span(mask, w, h, row,
							 xa, xb);
					span = 0;
				}
			}
		}
	}
	return 0;
}
