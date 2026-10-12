/* Software DRBG: see tls_rng.h. */

#include "tls_rng.h"
#include "tls_sha256.h"
#include "ucmd_lib.h"

static unsigned char state[32];
static unsigned char ctr[32];
static int seeded;

static void mix(const void *data, unsigned int len)
{
	struct sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, state, 32);
	if (len)
		sha256_update(&c, data, len);
	sha256_final(&c, state);
}

static void seed(void)
{
	struct sys_netinfo ni;
	unsigned char sample[64];
	unsigned int i;
	unsigned int t;

	for (i = 0; i < 32; i++)
		state[i] = (unsigned char)(0xA5 ^ i);

	/* RDTSC jitter: pairs of closely-spaced reads. */
	for (i = 0; i < 64; i += 4) {
		unsigned int lo1, lo2, hi;

		__asm__ volatile ("rdtsc" : "=a"(lo1), "=d"(hi));
		__asm__ volatile ("rdtsc" : "=a"(lo2), "=d"(hi));
		sample[i] = (unsigned char)lo1;
		sample[i + 1] = (unsigned char)(lo2 >> 5);
		sample[i + 2] = (unsigned char)(lo1 >> 13);
		sample[i + 3] = (unsigned char)(lo2 >> 21);
	}
	mix(sample, 64);

	t = (unsigned int)u_syscall0(SYS_GET_TICKS_MS);
	mix(&t, sizeof(t));

	if (u_syscall1(SYS_NET_INFO, (int)&ni) == 1)
		mix(ni.mac, 6);

	seeded = 1;
}

void tls_random(unsigned char *out, unsigned int len)
{
	struct sha256_ctx c;
	unsigned char block[32];
	unsigned int i, n;
	int j;

	if (!seeded)
		seed();

	while (len) {
		sha256_init(&c);
		sha256_update(&c, state, 32);
		sha256_update(&c, ctr, 32);
		sha256_final(&c, block);

		mix(block, 32);
		for (j = 31; j >= 0; j--) {      /* ctr++ (big endian) */
			if (++ctr[j] != 0)
				break;
		}

		n = len < 32 ? len : 32;
		for (i = 0; i < n; i++)
			out[i] = block[i];
		out += n;
		len -= n;
	}
}
