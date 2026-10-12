/* SHA-256 + HMAC-SHA256 + HKDF for the ring-3 TLS client.
 * Freestanding: no libc, no 64-bit division (i586, no libgcc). */
#ifndef TLS_SHA256_H
#define TLS_SHA256_H

struct sha256_ctx {
	unsigned int h[8];
	unsigned long long bits;   /* total message length in bits */
	unsigned char buf[64];
	unsigned int buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, unsigned int len);
void sha256_final(struct sha256_ctx *c, unsigned char out[32]);
void sha256_copy(struct sha256_ctx *dst, const struct sha256_ctx *src);
void sha256(const void *data, unsigned int len, unsigned char out[32]);

struct hmac_sha256_ctx {
	struct sha256_ctx inner;
	unsigned char okey[64];
};

void hmac_sha256_init(struct hmac_sha256_ctx *c, const unsigned char *key,
		      unsigned int key_len);
void hmac_sha256_update(struct hmac_sha256_ctx *c, const void *data,
			unsigned int len);
void hmac_sha256_final(struct hmac_sha256_ctx *c, unsigned char out[32]);
void hmac_sha256(const unsigned char *key, unsigned int key_len,
		 const void *data, unsigned int len, unsigned char out[32]);

/* HKDF (RFC 5869); salt may be NULL (treated as 32 zero bytes). */
void hkdf_extract(const unsigned char *salt, unsigned int salt_len,
		  const void *ikm, unsigned int ikm_len, unsigned char out[32]);
void hkdf_expand(const unsigned char prk[32], const void *info,
		 unsigned int info_len, unsigned char *out,
		 unsigned int out_len);

/* TLS 1.3 HKDF-Expand-Label. */
void hkdf_expand_label(const unsigned char secret[32], const char *label,
		       const unsigned char *ctx, unsigned int ctx_len,
		       unsigned char *out, unsigned int out_len);

#endif
