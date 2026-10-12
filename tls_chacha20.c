/* ChaCha20 + Poly1305 (RFC 8439): see tls_chacha20.h.
 * Poly1305 uses 5 x 26-bit limbs with 64-bit products (no __int128). */

#include "tls_chacha20.h"

static unsigned int rotl32(unsigned int x, int n)
{
	return (x << n) | (x >> (32 - n));
}

#define QR(a, b, c, d)                    \
	do {                              \
		a += b; d ^= a; d = rotl32(d, 16); \
		c += d; b ^= c; b = rotl32(b, 12); \
		a += b; d ^= a; d = rotl32(d, 8);  \
		c += d; b ^= c; b = rotl32(b, 7);  \
	} while (0)

static void chacha_block(const unsigned int in[16], unsigned char out[64])
{
	unsigned int x[16];
	int i;

	for (i = 0; i < 16; i++)
		x[i] = in[i];
	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]);
		QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]);
		QR(x[3], x[4], x[9], x[14]);
	}
	for (i = 0; i < 16; i++)
		x[i] += in[i];
	for (i = 0; i < 16; i++) {
		unsigned int v = x[i];
		out[i * 4] = (unsigned char)v;
		out[i * 4 + 1] = (unsigned char)(v >> 8);
		out[i * 4 + 2] = (unsigned char)(v >> 16);
		out[i * 4 + 3] = (unsigned char)(v >> 24);
	}
}

void chacha20_init(struct chacha20_ctx *c, const unsigned char key[32],
		   const unsigned char nonce[12], unsigned int counter)
{
	unsigned int i;

	c->st[0] = 0x61707865;
	c->st[1] = 0x3320646e;
	c->st[2] = 0x79622d32;
	c->st[3] = 0x6b206574;
	for (i = 0; i < 8; i++) {
		c->st[4 + i] = (unsigned int)key[i * 4] |
			       ((unsigned int)key[i * 4 + 1] << 8) |
			       ((unsigned int)key[i * 4 + 2] << 16) |
			       ((unsigned int)key[i * 4 + 3] << 24);
	}
	c->st[12] = counter;
	for (i = 0; i < 3; i++) {
		c->st[13 + i] = (unsigned int)nonce[i * 4] |
				((unsigned int)nonce[i * 4 + 1] << 8) |
				((unsigned int)nonce[i * 4 + 2] << 16) |
				((unsigned int)nonce[i * 4 + 3] << 24);
	}
	c->used = 64;
}

void chacha20_xor(struct chacha20_ctx *c, unsigned char *out,
		  const unsigned char *in, unsigned int len)
{
	while (len) {
		unsigned int n, i;

		if (c->used == 64) {
			chacha_block(c->st, c->block);
			c->st[12]++;
			c->used = 0;
		}
		n = 64 - c->used;
		if (n > len)
			n = len;
		for (i = 0; i < n; i++)
			out[i] = (unsigned char)(in[i] ^ c->block[c->used + i]);
		c->used += n;
		out += n;
		in += n;
		len -= n;
	}
}

/* ---- Poly1305 (donna-32 style) ---- */

#define P1305_MASK 0x3ffffff

static unsigned int pl_le32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void pl_store32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void poly1305_blocks(struct poly1305_ctx *c, const unsigned char *m,
			    unsigned int len, unsigned int hibit)
{
	unsigned int h0 = c->h[0], h1 = c->h[1], h2 = c->h[2];
	unsigned int h3 = c->h[3], h4 = c->h[4];
	unsigned int r0 = c->r[0], r1 = c->r[1], r2 = c->r[2];
	unsigned int r3 = c->r[3], r4 = c->r[4];
	unsigned int s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

	while (len >= 16) {
		unsigned int t0 = pl_le32(m), t1 = pl_le32(m + 4);
		unsigned int t2 = pl_le32(m + 8), t3 = pl_le32(m + 12);
		unsigned long long d0, d1, d2, d3, d4;
		unsigned int carry;

		h0 += t0 & P1305_MASK;
		h1 += ((t0 >> 26) | (t1 << 6)) & P1305_MASK;
		h2 += ((t1 >> 20) | (t2 << 12)) & P1305_MASK;
		h3 += ((t2 >> 14) | (t3 << 18)) & P1305_MASK;
		h4 += (t3 >> 8) | hibit;

		d0 = (unsigned long long)h0 * r0 +
		     (unsigned long long)h1 * s4 +
		     (unsigned long long)h2 * s3 +
		     (unsigned long long)h3 * s2 +
		     (unsigned long long)h4 * s1;
		d1 = (unsigned long long)h0 * r1 +
		     (unsigned long long)h1 * r0 +
		     (unsigned long long)h2 * s4 +
		     (unsigned long long)h3 * s3 +
		     (unsigned long long)h4 * s2;
		d2 = (unsigned long long)h0 * r2 +
		     (unsigned long long)h1 * r1 +
		     (unsigned long long)h2 * r0 +
		     (unsigned long long)h3 * s4 +
		     (unsigned long long)h4 * s3;
		d3 = (unsigned long long)h0 * r3 +
		     (unsigned long long)h1 * r2 +
		     (unsigned long long)h2 * r1 +
		     (unsigned long long)h3 * r0 +
		     (unsigned long long)h4 * s4;
		d4 = (unsigned long long)h0 * r4 +
		     (unsigned long long)h1 * r3 +
		     (unsigned long long)h2 * r2 +
		     (unsigned long long)h3 * r1 +
		     (unsigned long long)h4 * r0;

		carry = (unsigned int)(d0 >> 26);
		h0 = (unsigned int)d0 & P1305_MASK;
		d1 += carry;
		carry = (unsigned int)(d1 >> 26);
		h1 = (unsigned int)d1 & P1305_MASK;
		d2 += carry;
		carry = (unsigned int)(d2 >> 26);
		h2 = (unsigned int)d2 & P1305_MASK;
		d3 += carry;
		carry = (unsigned int)(d3 >> 26);
		h3 = (unsigned int)d3 & P1305_MASK;
		d4 += carry;
		carry = (unsigned int)(d4 >> 26);
		h4 = (unsigned int)d4 & P1305_MASK;
		h0 += carry * 5;
		carry = h0 >> 26;
		h0 &= P1305_MASK;
		h1 += carry;

		m += 16;
		len -= 16;
	}

	c->h[0] = h0;
	c->h[1] = h1;
	c->h[2] = h2;
	c->h[3] = h3;
	c->h[4] = h4;
}

void poly1305_init(struct poly1305_ctx *c, const unsigned char key[32])
{
	unsigned int t0 = pl_le32(key), t1 = pl_le32(key + 4);
	unsigned int t2 = pl_le32(key + 8), t3 = pl_le32(key + 12);

	c->r[0] = t0 & 0x3ffffff;
	c->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
	c->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
	c->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
	c->r[4] = (t3 >> 8) & 0x00fffff;
	c->pad[0] = pl_le32(key + 16);
	c->pad[1] = pl_le32(key + 20);
	c->pad[2] = pl_le32(key + 24);
	c->pad[3] = pl_le32(key + 28);
	c->h[0] = c->h[1] = c->h[2] = c->h[3] = c->h[4] = 0;
	c->leftover = 0;
}

void poly1305_update(struct poly1305_ctx *c, const unsigned char *m,
		     unsigned int len)
{
	unsigned int i;

	if (c->leftover) {
		unsigned int want = 16 - c->leftover;

		if (want > len)
			want = len;
		for (i = 0; i < want; i++)
			c->buf[c->leftover + i] = m[i];
		c->leftover += want;
		m += want;
		len -= want;
		if (c->leftover < 16)
			return;
		poly1305_blocks(c, c->buf, 16, 1u << 24);
		c->leftover = 0;
	}
	if (len >= 16) {
		unsigned int want = len & ~15u;

		poly1305_blocks(c, m, want, 1u << 24);
		m += want;
		len -= want;
	}
	if (len) {
		for (i = 0; i < len; i++)
			c->buf[i] = m[i];
		c->leftover = len;
	}
}

void poly1305_final(struct poly1305_ctx *c, unsigned char tag[16])
{
	unsigned int h0, h1, h2, h3, h4, g0, g1, g2, g3, g4, mask, i;
	unsigned long long d;

	if (c->leftover) {
		c->buf[c->leftover] = 1;
		for (i = c->leftover + 1; i < 16; i++)
			c->buf[i] = 0;
		poly1305_blocks(c, c->buf, 16, 0);
	}

	h0 = c->h[0];
	h1 = c->h[1];
	h2 = c->h[2];
	h3 = c->h[3];
	h4 = c->h[4];

	{
		unsigned int carry = h1 >> 26;
		h1 &= P1305_MASK;
		h2 += carry;
		carry = h2 >> 26;
		h2 &= P1305_MASK;
		h3 += carry;
		carry = h3 >> 26;
		h3 &= P1305_MASK;
		h4 += carry;
		carry = h4 >> 26;
		h4 &= P1305_MASK;
		h0 += carry * 5;
		carry = h0 >> 26;
		h0 &= P1305_MASK;
		h1 += carry;
	}

	g0 = h0 + 5;
	{
		unsigned int carry = g0 >> 26;
		g0 &= P1305_MASK;
		g1 = h1 + carry;
		carry = g1 >> 26;
		g1 &= P1305_MASK;
		g2 = h2 + carry;
		carry = g2 >> 26;
		g2 &= P1305_MASK;
		g3 = h3 + carry;
		carry = g3 >> 26;
		g3 &= P1305_MASK;
		g4 = h4 + carry - (1u << 26);
	}

	mask = (g4 >> 31) - 1;   /* all ones when no borrow (g4 valid) */
	g0 &= mask;
	g1 &= mask;
	g2 &= mask;
	g3 &= mask;
	g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	h0 = (h0 | (h1 << 26)) & 0xffffffffu;
	h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
	h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
	h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;

	d = (unsigned long long)h0 + c->pad[0];
	h0 = (unsigned int)d;
	d = (unsigned long long)h1 + c->pad[1] + (d >> 32);
	h1 = (unsigned int)d;
	d = (unsigned long long)h2 + c->pad[2] + (d >> 32);
	h2 = (unsigned int)d;
	d = (unsigned long long)h3 + c->pad[3] + (d >> 32);
	h3 = (unsigned int)d;

	pl_store32(tag, h0);
	pl_store32(tag + 4, h1);
	pl_store32(tag + 8, h2);
	pl_store32(tag + 12, h3);
}

/* ---- AEAD ---- */

static void poly_key_from_chacha(const unsigned char key[32],
				 const unsigned char nonce[12],
				 unsigned char out[32])
{
	struct chacha20_ctx c;
	unsigned char zero[64];
	unsigned int i;

	for (i = 0; i < 64; i++)
		zero[i] = 0;
	chacha20_init(&c, key, nonce, 0);
	chacha20_xor(&c, zero, zero, 64);
	for (i = 0; i < 32; i++)
		out[i] = zero[i];
}

void aead_open_init(struct aead_open_ctx *a, const unsigned char key[32],
		    const unsigned char nonce[12],
		    const unsigned char *aad, unsigned int aad_len)
{
	unsigned char pk[32];
	static const unsigned char zeros[16];

	poly_key_from_chacha(key, nonce, pk);
	poly1305_init(&a->poly, pk);
	poly1305_update(&a->poly, aad, aad_len);
	if (aad_len & 15)
		poly1305_update(&a->poly, zeros, 16 - (aad_len & 15));
	chacha20_init(&a->ch, key, nonce, 1);
	a->aad_len = aad_len;
	a->ct_len = 0;
}

void aead_open_update(struct aead_open_ctx *a, unsigned char *out,
		      const unsigned char *in, unsigned int len)
{
	poly1305_update(&a->poly, in, len);
	chacha20_xor(&a->ch, out, in, len);
	a->ct_len += len;
}

void aead_open_tag(struct aead_open_ctx *a, unsigned char tag[16])
{
	static const unsigned char zeros[16];
	unsigned char lens[16];
	unsigned int pad, i;

	pad = (16 - (a->ct_len & 15)) & 15;
	if (pad)
		poly1305_update(&a->poly, zeros, pad);

	for (i = 0; i < 8; i++)
		lens[i] = (unsigned char)((unsigned long long)a->aad_len >>
					  (i * 8));
	for (i = 0; i < 8; i++)
		lens[8 + i] = (unsigned char)((unsigned long long)a->ct_len >>
					      (i * 8));
	poly1305_update(&a->poly, lens, 16);
	poly1305_final(&a->poly, tag);
}

void aead_seal(const unsigned char key[32], const unsigned char nonce[12],
	       const unsigned char *aad, unsigned int aad_len,
	       const unsigned char *pt, unsigned int len,
	       unsigned char *ct, unsigned char tag[16])
{
	unsigned char pk[32];
	struct poly1305_ctx poly;
	struct chacha20_ctx ch;
	static const unsigned char zeros[16];
	unsigned char lens[16];
	unsigned int pad, i;

	poly_key_from_chacha(key, nonce, pk);
	poly1305_init(&poly, pk);
	poly1305_update(&poly, aad, aad_len);
	if (aad_len & 15)
		poly1305_update(&poly, zeros, 16 - (aad_len & 15));

	chacha20_init(&ch, key, nonce, 1);
	chacha20_xor(&ch, ct, pt, len);
	poly1305_update(&poly, ct, len);

	pad = (16 - (len & 15)) & 15;
	if (pad)
		poly1305_update(&poly, zeros, pad);
	for (i = 0; i < 8; i++)
		lens[i] = (unsigned char)((unsigned long long)aad_len >>
					  (i * 8));
	for (i = 0; i < 8; i++)
		lens[8 + i] = (unsigned char)((unsigned long long)len >>
					      (i * 8));
	poly1305_update(&poly, lens, 16);
	poly1305_final(&poly, tag);
}
