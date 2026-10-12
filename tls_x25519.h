/* X25519 (RFC 7748) for the ring-3 TLS client.
 * Field arithmetic in 8 x 32-bit limbs (no __int128 on i586). */
#ifndef TLS_X25519_H
#define TLS_X25519_H

/* out = scalar * point (all 32-byte little-endian; scalar is clamped). */
void x25519(unsigned char out[32], const unsigned char scalar[32],
	    const unsigned char point[32]);

extern const unsigned char x25519_base_point[32];

#endif
