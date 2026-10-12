/* ChaCha20-Poly1305 AEAD (RFC 8439) for the ring-3 TLS client.
 * The open path is incremental (ciphertext arrives in TCP-sized chunks);
 * the seal path is one-shot (everything we send is small). */
#ifndef TLS_CHACHA20_H
#define TLS_CHACHA20_H

struct chacha20_ctx {
	unsigned int st[16];
	unsigned char block[64];
	unsigned int used;              /* bytes consumed from block (64 = none) */
};

void chacha20_init(struct chacha20_ctx *c, const unsigned char key[32],
		   const unsigned char nonce[12], unsigned int counter);
void chacha20_xor(struct chacha20_ctx *c, unsigned char *out,
		  const unsigned char *in, unsigned int len);

struct poly1305_ctx {
	unsigned int r[5], h[5];
	unsigned int pad[4];
	unsigned int leftover;
	unsigned char buf[16];
};

void poly1305_init(struct poly1305_ctx *c, const unsigned char key[32]);
void poly1305_update(struct poly1305_ctx *c, const unsigned char *m,
		     unsigned int len);
void poly1305_final(struct poly1305_ctx *c, unsigned char tag[16]);

/* Incremental AEAD open: init with the key/nonce and the record header as
 * AAD, feed ciphertext chunks, then read the tag to compare. */
struct aead_open_ctx {
	struct chacha20_ctx ch;
	struct poly1305_ctx poly;
	unsigned int aad_len;
	unsigned int ct_len;
};

void aead_open_init(struct aead_open_ctx *a, const unsigned char key[32],
		    const unsigned char nonce[12],
		    const unsigned char *aad, unsigned int aad_len);
void aead_open_update(struct aead_open_ctx *a, unsigned char *out,
		      const unsigned char *in, unsigned int len);
void aead_open_tag(struct aead_open_ctx *a, unsigned char tag[16]);

/* One-shot seal: ct[0..len-1] and the 16-byte tag. */
void aead_seal(const unsigned char key[32], const unsigned char nonce[12],
	       const unsigned char *aad, unsigned int aad_len,
	       const unsigned char *pt, unsigned int len,
	       unsigned char *ct, unsigned char tag[16]);

#endif
