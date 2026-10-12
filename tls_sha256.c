/* SHA-256 + HMAC-SHA256 + HKDF: see tls_sha256.h. */

#include "tls_sha256.h"

static const unsigned int K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static unsigned int rotr(unsigned int x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256_ctx *c, const unsigned char *p)
{
	unsigned int w[64], a, b, d, e, f, g, h, t1, t2;
	unsigned int cc;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((unsigned int)p[i * 4] << 24) |
		       ((unsigned int)p[i * 4 + 1] << 16) |
		       ((unsigned int)p[i * 4 + 2] << 8) |
		       (unsigned int)p[i * 4 + 3];
	}
	for (; i < 64; i++) {
		unsigned int s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
				  (w[i - 15] >> 3);
		unsigned int s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
				  (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = c->h[0];
	b = c->h[1];
	cc = c->h[2];
	d = c->h[3];
	e = c->h[4];
	f = c->h[5];
	g = c->h[6];
	h = c->h[7];

	for (i = 0; i < 64; i++) {
		unsigned int s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		unsigned int ch = (e & f) ^ (~e & g);
		unsigned int s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		unsigned int maj = (a & b) ^ (a & cc) ^ (b & cc);

		t1 = h + s1 + ch + K[i] + w[i];
		t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = cc;
		cc = b;
		b = a;
		a = t1 + t2;
	}

	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
	c->h[5] += f;
	c->h[6] += g;
	c->h[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
	c->h[0] = 0x6a09e667;
	c->h[1] = 0xbb67ae85;
	c->h[2] = 0x3c6ef372;
	c->h[3] = 0xa54ff53a;
	c->h[4] = 0x510e527f;
	c->h[5] = 0x9b05688c;
	c->h[6] = 0x1f83d9ab;
	c->h[7] = 0x5be0cd19;
	c->bits = 0;
	c->buflen = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, unsigned int len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int i;

	c->bits += (unsigned long long)len * 8;

	if (c->buflen) {
		unsigned int want = 64 - c->buflen;

		if (want > len)
			want = len;
		for (i = 0; i < want; i++)
			c->buf[c->buflen + i] = p[i];
		c->buflen += want;
		p += want;
		len -= want;
		if (c->buflen < 64)
			return;
		sha256_block(c, c->buf);
		c->buflen = 0;
	}
	while (len >= 64) {
		sha256_block(c, p);
		p += 64;
		len -= 64;
	}
	for (i = 0; i < len; i++)
		c->buf[i] = p[i];
	c->buflen = len;
}

void sha256_final(struct sha256_ctx *c, unsigned char out[32])
{
	unsigned long long bits = c->bits;
	unsigned int i;

	c->buf[c->buflen++] = 0x80;
	if (c->buflen > 56) {
		while (c->buflen < 64)
			c->buf[c->buflen++] = 0;
		sha256_block(c, c->buf);
		c->buflen = 0;
	}
	while (c->buflen < 56)
		c->buf[c->buflen++] = 0;
	for (i = 0; i < 8; i++)
		c->buf[56 + i] = (unsigned char)(bits >> (56 - i * 8));
	sha256_block(c, c->buf);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (unsigned char)(c->h[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
		out[i * 4 + 3] = (unsigned char)c->h[i];
	}
}

void sha256_copy(struct sha256_ctx *dst, const struct sha256_ctx *src)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		dst->h[i] = src->h[i];
	dst->bits = src->bits;
	dst->buflen = src->buflen;
	for (i = 0; i < 64; i++)
		dst->buf[i] = src->buf[i];
}

void sha256(const void *data, unsigned int len, unsigned char out[32])
{
	struct sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, data, len);
	sha256_final(&c, out);
}

/* ---- HMAC ---- */

void hmac_sha256_init(struct hmac_sha256_ctx *c, const unsigned char *key,
		      unsigned int key_len)
{
	unsigned char k[64], kh[32], pad[64];
	unsigned int i;

	for (i = 0; i < 64; i++)
		k[i] = 0;
	if (key_len > 64) {
		sha256(key, key_len, kh);
		for (i = 0; i < 32; i++)
			k[i] = kh[i];
	} else {
		for (i = 0; i < key_len; i++)
			k[i] = key[i];
	}

	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&c->inner);
	sha256_update(&c->inner, pad, 64);
	for (i = 0; i < 64; i++)
		c->okey[i] = k[i] ^ 0x5c;
}

void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data,
			unsigned int len)
{
	sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(struct hmac_sha256_ctx *c, unsigned char out[32])
{
	struct sha256_ctx outer;
	unsigned char ih[32];

	sha256_final(&c->inner, ih);
	sha256_init(&outer);
	sha256_update(&outer, c->okey, 64);
	sha256_update(&outer, ih, 32);
	sha256_final(&outer, out);
}

/* ---- HKDF ---- */

void hmac_sha256(const unsigned char *key, unsigned int key_len,
		 const void *data, unsigned int len, unsigned char out[32])
{
	struct hmac_sha256_ctx h;

	hmac_sha256_init(&h, key, key_len);
	hmac_sha256_update(&h, data, len);
	hmac_sha256_final(&h, out);
}

void hkdf_extract(const unsigned char *salt, unsigned int salt_len,
		  const void *ikm, unsigned int ikm_len, unsigned char out[32])
{
	unsigned char zero[32];
	unsigned int i;

	if (!salt) {
		for (i = 0; i < 32; i++)
			zero[i] = 0;
		salt = zero;
		salt_len = 32;
	}
	hmac_sha256(salt, salt_len, ikm, ikm_len, out);
}

void hkdf_expand(const unsigned char prk[32], const void *info,
		 unsigned int info_len, unsigned char *out,
		 unsigned int out_len)
{
	unsigned char t[32];
	unsigned int tlen = 0, pos = 0;
	unsigned char ctr = 1;

	if (out_len == 0)
		return;
	while (pos < out_len) {
		struct hmac_sha256_ctx h;
		unsigned int avail, k;

		hmac_sha256_init(&h, prk, 32);
		if (tlen)
			hmac_sha256_update(&h, t, tlen);
		if (info_len)
			hmac_sha256_update(&h, info, info_len);
		hmac_sha256_update(&h, &ctr, 1);
		hmac_sha256_final(&h, t);
		tlen = 32;
		ctr++;

		avail = out_len - pos;
		if (avail > 32)
			avail = 32;
		for (k = 0; k < avail; k++)
			out[pos + k] = t[k];
		pos += avail;
	}
}

void hkdf_expand_label(const unsigned char secret[32], const char *label,
		       const unsigned char *ctx, unsigned int ctx_len,
		       unsigned char *out, unsigned int out_len)
{
	static const char prefix[] = "tls13 ";
	unsigned char info[96];
	unsigned int o = 0, n = 0, i;

	while (label[n])
		n++;

	info[o++] = (unsigned char)(out_len >> 8);
	info[o++] = (unsigned char)out_len;
	info[o++] = (unsigned char)(6 + n);
	for (i = 0; i < 6; i++)
		info[o++] = (unsigned char)prefix[i];
	for (i = 0; i < n; i++)
		info[o++] = (unsigned char)label[i];
	info[o++] = (unsigned char)ctx_len;
	for (i = 0; i < ctx_len; i++)
		info[o++] = ctx[i];

	hkdf_expand(secret, info, o, out, out_len);
}
