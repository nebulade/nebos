/* inflate: see inflate.h.  A compact, bounds-checked DEFLATE decoder.
 *
 * The Huffman decoder is the canonical-code bit-at-a-time form; the LZ77
 * history is a 32 KB circular window that is also used as the output staging
 * area, so the caller's sink is called in chunks (never per byte). */

#include "inflate.h"

#define MAXBITS   INFLATE_MAXBITS
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288
#define FASTBITS  9

static unsigned int crc_table[256];
static int crc_table_ready;

static void crc_table_init(void)
{
	unsigned int n, k, c;

	if (crc_table_ready)
		return;
	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[n] = c;
	}
	crc_table_ready = 1;
}

static unsigned int crc_update(unsigned int crc, const unsigned char *p,
			       unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		crc ^= p[i], crc = (crc >> 8) ^ crc_table[crc & 0xFF];
	return crc;
}

static void adler_update(unsigned int *a, unsigned int *b,
			 const unsigned char *p, unsigned int len)
{
	unsigned int n;

	while (len) {
		n = len > 5552 ? 5552 : len;
		len -= n;
		while (n--) {
			*a += *p++;
			*b += *a;
		}
		*a %= 65521;
		*b %= 65521;
	}
}

unsigned int crc32_bytes(const void *data, unsigned int len)
{
	crc_table_init();
	return crc_update(0xFFFFFFFFu, (const unsigned char *)data, len) ^
	       0xFFFFFFFFu;
}

unsigned int adler32_bytes(const void *data, unsigned int len)
{
	unsigned int a = 1, b = 0;

	adler_update(&a, &b, (const unsigned char *)data, len);
	return (b << 16) | a;
}

/* ---- source ---------------------------------------------------------- */

static int src_byte(struct inflate_src *s, unsigned int *out)
{
	while (s->len == 0) {
		const unsigned char *p;
		unsigned int len;

		if (!s->more || !s->more(s->ctx, &p, &len) || len == 0)
			return -1;
		s->p = p;
		s->len = len;
	}
	*out = *s->p++;
	s->len--;
	return 0;
}

/* ---- output ---------------------------------------------------------- */

static int flush_out(struct inflate *z)
{
	unsigned int start, first, rest;
	int r;

	if (z->pending == 0)
		return 0;
	start = (z->wpos + 32768u - z->pending) & 32767u;
	first = z->pending;
	if (start + first > 32768u)
		first = 32768u - start;
	rest = z->pending - first;

	z->crc = crc_update(z->crc, z->win + start, first);
	adler_update(&z->adler_a, &z->adler_b, z->win + start, first);
	if (rest) {
		z->crc = crc_update(z->crc, z->win, rest);
		adler_update(&z->adler_a, &z->adler_b, z->win, rest);
	}

	r = z->sink.write(z->sink.ctx, z->win + start, first);
	if (!r && rest)
		r = z->sink.write(z->sink.ctx, z->win, rest);
	if (r)
		return -1;
	z->pending = 0;
	if (z->progress)
		z->progress(z->progress_ctx);
	return 0;
}

static int put_byte(struct inflate *z, unsigned int b)
{
	z->win[z->wpos] = (unsigned char)b;
	z->wpos = (z->wpos + 1) & 32767u;
	z->total++;
	if (++z->pending >= 16384)
		return flush_out(z);
	return 0;
}

/* ---- bit reader ------------------------------------------------------ */

static int need_bits(struct inflate *z, int n)
{
	while (z->bitcnt < n) {
		unsigned int b;

		if (src_byte(&z->src, &b))
			return -1;
		z->bitbuf |= b << z->bitcnt;
		z->bitcnt += 8;
	}
	return 0;
}

static int get_bits(struct inflate *z, int n)
{
	unsigned int v;

	if (n == 0)
		return 0;
	if (need_bits(z, n))
		return -1;
	v = z->bitbuf & ((1u << n) - 1u);
	z->bitbuf >>= n;
	z->bitcnt -= n;
	z->total_bits += (unsigned int)n;
	return (int)v;
}

static int align_byte(struct inflate *z)
{
	int pad = (int)((8u - (unsigned int)(z->total_bits & 7u)) & 7u);

	if (pad && get_bits(z, pad) < 0)
		return -1;
	return 0;
}

/* ---- Huffman --------------------------------------------------------- */

static int huff_build(struct inflate_huff *h, const unsigned char *lengths,
		      int n)
{
	unsigned short offs[MAXBITS + 1];
	int len, sym, left;

	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (sym = 0; sym < n; sym++)
		h->count[lengths[sym]]++;
	if (h->count[0] == n)
		return 0;       /* no codes: caller rejects if it needs one */

	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return -1;  /* over-subscribed */
	}

	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = (unsigned short)(offs[len] + h->count[len]);
	for (sym = 0; sym < n; sym++)
		if (lengths[sym])
			h->symbol[offs[lengths[sym]]++] = (unsigned short)sym;
	return 1;
}

static int huff_decode(struct inflate *z, const struct inflate_huff *h)
{
	int len, code = 0, first = 0, index = 0;

	for (len = 1; len <= MAXBITS; len++) {
		int b = get_bits(z, 1);
		int count;

		if (b < 0)
			return -1;
		code |= b;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* ---- DEFLATE blocks -------------------------------------------------- */

static const unsigned short len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_ext[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
	193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_ext[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static int copy_match(struct inflate *z, unsigned int len, unsigned int dist)
{
	if (dist == 0 || dist > 32768u || len > 258u)
		return -1;
	while (len--) {
		unsigned int src = (z->wpos + 32768u - dist) & 32767u;

		if (put_byte(z, z->win[src]))
			return -1;
	}
	return 0;
}

static int codes(struct inflate *z, const struct inflate_huff *lencode,
		 const struct inflate_huff *distcode)
{
	for (;;) {
		int sym = huff_decode(z, lencode);
		int len, dsym, dist;

		if (sym < 0)
			return -1;
		if (sym < 256) {
			if (put_byte(z, (unsigned int)sym))
				return -1;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29)
				return -1;
			len = len_base[sym];
			if (len_ext[sym]) {
				int e = get_bits(z, len_ext[sym]);

				if (e < 0)
					return -1;
				len += e;
			}
			dsym = huff_decode(z, distcode);
			if (dsym < 0 || dsym >= 30)
				return -1;
			dist = dist_base[dsym];
			if (dist_ext[dsym]) {
				int e = get_bits(z, dist_ext[dsym]);

				if (e < 0)
					return -1;
				dist += e;
			}
			if (copy_match(z, (unsigned int)len, (unsigned int)dist))
				return -1;
		}
	}
}

static int stored_block(struct inflate *z)
{
	int len, nlen, i;

	len = get_bits(z, 16);
	nlen = get_bits(z, 16);
	if (len < 0 || nlen < 0 || (len ^ nlen) != 0xFFFF)
		return -1;
	for (i = 0; i < len; i++) {
		int b = get_bits(z, 8);

		if (b < 0 || put_byte(z, (unsigned int)b))
			return -1;
	}
	return 0;
}

static int fixed_block(struct inflate *z)
{
	unsigned char lengths[FIXLCODES];
	static const unsigned char dist_lengths[30] = {
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5
	};
	struct inflate_huff lencode, distcode;
	int i;

	for (i = 0; i < 144; i++)
		lengths[i] = 8;
	for (; i < 256; i++)
		lengths[i] = 9;
	for (; i < 280; i++)
		lengths[i] = 7;
	for (; i < 288; i++)
		lengths[i] = 8;
	if (huff_build(&lencode, lengths, 288) < 0 ||
	    huff_build(&distcode, dist_lengths, 30) < 0)
		return -1;
	return codes(z, &lencode, &distcode);
}

static int dynamic_block(struct inflate *z)
{
	static const unsigned char order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	unsigned char lengths[MAXCODES];
	struct inflate_huff lencode, distcode;
	int hlit, hdist, hclen, i, total;

	hlit = get_bits(z, 5);
	hdist = get_bits(z, 5);
	hclen = get_bits(z, 4);
	if (hlit < 0 || hdist < 0 || hclen < 0)
		return -1;
	hlit += 257;
	hdist += 1;
	hclen += 4;
	if (hlit > MAXLCODES || hdist > MAXDCODES)
		return -1;

	for (i = 0; i < 19; i++)
		lengths[i] = 0;
	for (i = 0; i < hclen; i++) {
		int v = get_bits(z, 3);

		if (v < 0)
			return -1;
		lengths[order[i]] = (unsigned char)v;
	}
	if (huff_build(&lencode, lengths, 19) < 0)
		return -1;

	total = hlit + hdist;
	for (i = 0; i < total; ) {
		int sym = huff_decode(z, &lencode);
		int rep, val;

		if (sym < 0)
			return -1;
		if (sym < 16) {
			lengths[i++] = (unsigned char)sym;
			continue;
		}
		if (sym == 16) {
			if (i == 0)
				return -1;
			rep = get_bits(z, 2);
			if (rep < 0)
				return -1;
			rep += 3;
			val = lengths[i - 1];
		} else if (sym == 17) {
			rep = get_bits(z, 3);
			if (rep < 0)
				return -1;
			rep += 3;
			val = 0;
		} else if (sym == 18) {
			rep = get_bits(z, 7);
			if (rep < 0)
				return -1;
			rep += 11;
			val = 0;
		} else {
			return -1;
		}
		if (i + rep > total)
			return -1;
		while (rep--)
			lengths[i++] = (unsigned char)val;
	}

	if (huff_build(&lencode, lengths, hlit) < 0)
		return -1;
	if (huff_build(&distcode, lengths + hlit, hdist) < 0)
		return -1;
	return codes(z, &lencode, &distcode);
}

void inflate_init(struct inflate *z, struct inflate_src *src,
		  struct inflate_sink *sink)
{
	unsigned char *p = (unsigned char *)z;
	unsigned int i;

	crc_table_init();
	for (i = 0; i < sizeof(*z); i++)
		p[i] = 0;
	z->src = *src;
	z->sink = *sink;
	z->crc = 0xFFFFFFFFu;
	z->adler_a = 1;
	z->adler_b = 0;
}

static int inflate_blocks(struct inflate *z)
{
	for (;;) {
		int last = get_bits(z, 1);
		int type = get_bits(z, 2);
		int r;

		if (last < 0 || type < 0)
			return -1;
		if (type == 0) {
			if (align_byte(z))
				return -1;
			r = stored_block(z);
		} else if (type == 1) {
			r = fixed_block(z);
		} else if (type == 2) {
			r = dynamic_block(z);
		} else {
			return -1;
		}
		if (r)
			return -1;
		if (last)
			break;
	}
	return flush_out(z);
}

int inflate_raw(struct inflate *z, unsigned int *out_len)
{
	if (inflate_blocks(z))
		return -1;
	if (out_len)
		*out_len = (unsigned int)z->total;
	return 0;
}

int inflate_zlib(struct inflate *z, unsigned int *out_len)
{
	unsigned int cmf, flg, want;
	int i;

	if (src_byte(&z->src, &cmf) || src_byte(&z->src, &flg))
		return -1;
	if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7)
		return -1;
	if (((cmf << 8) | flg) % 31 != 0)
		return -1;
	if (flg & 0x20)         /* preset dictionary: not supported */
		return -1;

	if (inflate_blocks(z))
		return -1;
	if (align_byte(z))
		return -1;
	want = 0;
	for (i = 0; i < 4; i++) {
		int b = get_bits(z, 8);

		if (b < 0)
			return -1;
		want = (want << 8) | (unsigned int)b;   /* zlib is big-endian */
	}
	if (want != ((z->adler_b << 16) | z->adler_a))
		return -1;
	if (out_len)
		*out_len = (unsigned int)z->total;
	return 0;
}

int inflate_gzip(struct inflate *z, unsigned int *out_len)
{
	unsigned int id1, id2, cm, flg, b, skip, want_crc, want_size;
	int i;

	if (src_byte(&z->src, &id1) || src_byte(&z->src, &id2) ||
	    src_byte(&z->src, &cm) || src_byte(&z->src, &flg))
		return -1;
	if (id1 != 0x1F || id2 != 0x8B || cm != 8)
		return -1;
	for (i = 0; i < 6; i++)       /* MTIME, XFL, OS */
		if (src_byte(&z->src, &b))
			return -1;
	if (flg & 0x04) {             /* FEXTRA */
		unsigned int lo, hi;

		if (src_byte(&z->src, &lo) || src_byte(&z->src, &hi))
			return -1;
		skip = lo | (hi << 8);
		while (skip--)
			if (src_byte(&z->src, &b))
				return -1;
	}
	if (flg & 0x08) {             /* FNAME */
		do {
			if (src_byte(&z->src, &b))
				return -1;
		} while (b);
	}
	if (flg & 0x10) {             /* FCOMMENT */
		do {
			if (src_byte(&z->src, &b))
				return -1;
		} while (b);
	}
	if (flg & 0x02) {             /* FHCRC */
		if (src_byte(&z->src, &b) || src_byte(&z->src, &b))
			return -1;
	}

	if (inflate_blocks(z))
		return -1;
	if (align_byte(z))
		return -1;

	/* The trailer: CRC32 then ISIZE, both little-endian. */
	want_crc = 0;
	for (i = 0; i < 4; i++) {
		int v = get_bits(z, 8);

		if (v < 0)
			return -1;
		want_crc |= (unsigned int)v << (8 * i);
	}
	want_size = 0;
	for (i = 0; i < 4; i++) {
		int v = get_bits(z, 8);

		if (v < 0)
			return -1;
		want_size |= (unsigned int)v << (8 * i);
	}
	if ((want_crc ^ 0xFFFFFFFFu) != z->crc)
		return -1;
	if (want_size != (unsigned int)z->total)
		return -1;
	if (out_len)
		*out_len = (unsigned int)z->total;
	return 0;
}

/* ---- whole-buffer helpers ------------------------------------------- */

struct mem_sink {
	unsigned char *dst;
	unsigned int cap, pos;
};

static int mem_write(void *ctx, const unsigned char *data, unsigned int len)
{
	struct mem_sink *m = (struct mem_sink *)ctx;

	if (len > m->cap - m->pos)
		return -1;
	{
		unsigned int i;

		for (i = 0; i < len; i++)
			m->dst[m->pos + i] = data[i];
	}
	m->pos += len;
	return 0;
}

static int mem_inflate(const void *src, unsigned int len, void *dst,
		       unsigned int cap, unsigned int *out_len, void *ws,
		       int gzip)
{
	struct inflate *z = (struct inflate *)ws;
	struct inflate_src s;
	struct inflate_sink sink;
	struct mem_sink m;

	s.p = (const unsigned char *)src;
	s.len = len;
	s.more = 0;
	s.ctx = 0;
	m.dst = (unsigned char *)dst;
	m.cap = cap;
	m.pos = 0;
	sink.write = mem_write;
	sink.ctx = &m;
	inflate_init(z, &s, &sink);
	if (gzip ? inflate_gzip(z, out_len) : inflate_zlib(z, out_len))
		return -1;
	return 0;
}

int inflate_zlib_mem(const void *src, unsigned int len, void *dst,
		     unsigned int cap, unsigned int *out_len, void *ws)
{
	return mem_inflate(src, len, dst, cap, out_len, ws, 0);
}

int inflate_gzip_mem(const void *src, unsigned int len, void *dst,
		     unsigned int cap, unsigned int *out_len, void *ws)
{
	return mem_inflate(src, len, dst, cap, out_len, ws, 1);
}

int inflate_raw_mem(const void *src, unsigned int len, void *dst,
		    unsigned int cap, unsigned int *out_len, void *ws)
{
	struct inflate *z = (struct inflate *)ws;
	struct inflate_src s;
	struct inflate_sink sink;
	struct mem_sink m;

	s.p = (const unsigned char *)src;
	s.len = len;
	s.more = 0;
	s.ctx = 0;
	m.dst = (unsigned char *)dst;
	m.cap = cap;
	m.pos = 0;
	sink.write = mem_write;
	sink.ctx = &m;
	inflate_init(z, &s, &sink);
	return inflate_raw(z, out_len);
}
