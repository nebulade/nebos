/* Software DRBG for the ring-3 TLS client: SHA-256 in counter mode, lazily
 * seeded from RDTSC jitter, the tick counter and the NIC MAC.
 *
 * Deliberately simple: good enough for unique handshake randoms and
 * ephemeral X25519 keys, but NOT strong against a determined attacker
 * (comparable to the rest of the phase-A "trust everyone" trade-off). */
#ifndef TLS_RNG_H
#define TLS_RNG_H

void tls_random(unsigned char *out, unsigned int len);

#endif
