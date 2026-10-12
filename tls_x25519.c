/* X25519: see tls_x25519.h.
 *
 * The field elements are kept below 2^255: adds wrap with 2^256 == 38 and
 * subtracts borrow-fix with the same identity, so a single conditional
 * subtraction of p restores the invariant.  Multiplication is 8x8 u32
 * schoolbook with per-step carries, then the high half is folded with *38
 * (2^256 mod p).  Slow but simple and testable against RFC 7748. */

#include "tls_x25519.h"

struct fe {
	unsigned int v[8];
};

const unsigned char x25519_base_point[32] = { 9 };

static unsigned int fe_ld32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void fe_st32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void fe_frombytes(struct fe *r, const unsigned char b[32])
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		r->v[i] = fe_ld32(b + i * 4);
	r->v[7] &= 0x7fffffffu;
}

static void fe_tobytes(unsigned char out[32], const struct fe *a)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		fe_st32(out + i * 4, a->v[i]);
}

static void fe_copy(struct fe *r, const struct fe *a)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		r->v[i] = a->v[i];
}

static void fe_zero(struct fe *r)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		r->v[i] = 0;
}

static void fe_one(struct fe *r)
{
	fe_zero(r);
	r->v[0] = 1;
}

/* r -= p (2^255 - 19); the caller guarantees r >= p. */
static void fe_sub_p(struct fe *r)
{
	unsigned long long d = 0;
	unsigned int i;
	unsigned int borrow = 0;

	for (i = 0; i < 8; i++) {
		unsigned int p = (i == 7) ? 0x7fffffffu :
				 (i == 0) ? 0xffffffedu : 0xffffffffu;
		d = (unsigned long long)r->v[i] - p - borrow;
		r->v[i] = (unsigned int)d;
		borrow = (unsigned int)((d >> 32) & 1);
	}
}

/* If the value is >= 2^255, subtract p once (result < 2^255). */
static void fe_reduce1(struct fe *r)
{
	if (r->v[7] >> 31)
		fe_sub_p(r);
}

static void fe_add(struct fe *r, const struct fe *a, const struct fe *b)
{
	unsigned long long carry = 0;
	unsigned int i;

	for (i = 0; i < 8; i++) {
		carry += (unsigned long long)a->v[i] + b->v[i];
		r->v[i] = (unsigned int)carry;
		carry >>= 32;
	}
	while (carry) {
		carry = (unsigned long long)r->v[0] + carry * 38;
		r->v[0] = (unsigned int)carry;
		carry >>= 32;
		for (i = 1; i < 8 && carry; i++) {
			carry += r->v[i];
			r->v[i] = (unsigned int)carry;
			carry >>= 32;
		}
	}
	fe_reduce1(r);
}

static void fe_sub(struct fe *r, const struct fe *a, const struct fe *b)
{
	unsigned long long d = 0;
	unsigned int i;
	unsigned int borrow = 0;

	for (i = 0; i < 8; i++) {
		d = (unsigned long long)a->v[i] - b->v[i] - borrow;
		r->v[i] = (unsigned int)d;
		borrow = (unsigned int)((d >> 32) & 1);
	}
	if (borrow) {
		/* a - b + 2^256 == a - b + 38 (mod p): subtract the 38. */
		d = (unsigned long long)r->v[0] - 38;
		r->v[0] = (unsigned int)d;
		borrow = (unsigned int)((d >> 32) & 1);
		for (i = 1; i < 8 && borrow; i++) {
			d = (unsigned long long)r->v[i] - borrow;
			r->v[i] = (unsigned int)d;
			borrow = (unsigned int)((d >> 32) & 1);
		}
	}
	fe_reduce1(r);
}

static void fe_mul(struct fe *r, const struct fe *a, const struct fe *b)
{
	unsigned int t[16];
	unsigned long long u[8];
	unsigned long long carry;
	unsigned int i, j;

	for (i = 0; i < 16; i++)
		t[i] = 0;
	for (i = 0; i < 8; i++) {
		carry = 0;
		for (j = 0; j < 8; j++) {
			carry += (unsigned long long)a->v[i] * b->v[j] +
				 t[i + j];
			t[i + j] = (unsigned int)carry;
			carry >>= 32;
		}
		t[i + 8] = (unsigned int)carry;
	}

	/* Fold the limbs above 2^256: 2^256 == 38 (mod p). */
	for (j = 0; j < 8; j++)
		u[j] = (unsigned long long)t[j] + 38ull * t[j + 8];
	carry = 0;
	for (j = 0; j < 8; j++) {
		carry += u[j];
		r->v[j] = (unsigned int)carry;
		carry >>= 32;
	}
	while (carry) {
		carry = (unsigned long long)r->v[0] + carry * 38;
		r->v[0] = (unsigned int)carry;
		carry >>= 32;
		for (j = 1; j < 8 && carry; j++) {
			carry += r->v[j];
			r->v[j] = (unsigned int)carry;
			carry >>= 32;
		}
	}
	fe_reduce1(r);
}

static void fe_sq(struct fe *r, const struct fe *a)
{
	fe_mul(r, a, a);
}

static void fe_mul_small(struct fe *r, const struct fe *a, unsigned int k)
{
	unsigned long long carry = 0;
	unsigned int i;

	for (i = 0; i < 8; i++) {
		carry += (unsigned long long)a->v[i] * k;
		r->v[i] = (unsigned int)carry;
		carry >>= 32;
	}
	while (carry) {
		carry = (unsigned long long)r->v[0] + carry * 38;
		r->v[0] = (unsigned int)carry;
		carry >>= 32;
		for (i = 1; i < 8 && carry; i++) {
			carry += r->v[i];
			r->v[i] = (unsigned int)carry;
			carry >>= 32;
		}
	}
	fe_reduce1(r);
}

/* r = a^-1 = a^(p-2); generic square-and-multiply over 2^255 - 21. */
static void fe_invert(struct fe *r, const struct fe *a)
{
	static const unsigned int exp[8] = {
		0xffffffebu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
		0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu
	};
	struct fe base, acc;
	int i;

	fe_copy(&base, a);
	fe_one(&acc);
	for (i = 254; i >= 0; i--) {
		fe_sq(&acc, &acc);
		if ((exp[i >> 5] >> (i & 31)) & 1)
			fe_mul(&acc, &acc, &base);
	}
	fe_copy(r, &acc);
}

static void fe_cswap(struct fe *a, struct fe *b, unsigned int swap)
{
	unsigned int mask = 0u - swap;
	unsigned int i;

	for (i = 0; i < 8; i++) {
		unsigned int t = mask & (a->v[i] ^ b->v[i]);

		a->v[i] ^= t;
		b->v[i] ^= t;
	}
}

/* Canonicalize: one conditional subtraction of p (input < 2^255). */
static void fe_canon(struct fe *r)
{
	struct fe t;
	unsigned int i;
	unsigned int borrow = 0;

	for (i = 0; i < 8; i++) {
		unsigned int p = (i == 7) ? 0x7fffffffu :
				 (i == 0) ? 0xffffffedu : 0xffffffffu;
		unsigned long long d = (unsigned long long)r->v[i] - p - borrow;

		t.v[i] = (unsigned int)d;
		borrow = (unsigned int)((d >> 32) & 1);
	}
	if (!borrow)
		fe_copy(r, &t);
}

void x25519(unsigned char out[32], const unsigned char scalar[32],
	    const unsigned char point[32])
{
	unsigned char k[32];
	struct fe x1, x2, z2, x3, z3;
	struct fe A, AA, B, BB, E, C, D, DA, CB, t0, t1;
	int i;
	unsigned int swap = 0;

	for (i = 0; i < 32; i++)
		k[i] = scalar[i];
	k[0] &= 248;
	k[31] &= 127;
	k[31] |= 64;

	fe_frombytes(&x1, point);
	fe_one(&x2);
	fe_zero(&z2);
	fe_copy(&x3, &x1);
	fe_one(&z3);

	for (i = 254; i >= 0; i--) {
		unsigned int kt = (k[(unsigned int)i >> 3] >>
				   ((unsigned int)i & 7)) & 1;

		swap ^= kt;
		fe_cswap(&x2, &x3, swap);
		fe_cswap(&z2, &z3, swap);
		swap = kt;

		fe_add(&A, &x2, &z2);
		fe_sq(&AA, &A);
		fe_sub(&B, &x2, &z2);
		fe_sq(&BB, &B);
		fe_sub(&E, &AA, &BB);
		fe_add(&C, &x3, &z3);
		fe_sub(&D, &x3, &z3);
		fe_mul(&DA, &D, &A);
		fe_mul(&CB, &C, &B);

		fe_add(&t0, &DA, &CB);
		fe_sq(&x3, &t0);
		fe_sub(&t1, &DA, &CB);
		fe_sq(&t1, &t1);
		fe_mul(&z3, &x1, &t1);

		fe_mul(&x2, &AA, &BB);
		fe_mul_small(&t0, &E, 121665);
		fe_add(&t0, &AA, &t0);
		fe_mul(&z2, &E, &t0);
	}
	fe_cswap(&x2, &x3, swap);
	fe_cswap(&z2, &z3, swap);

	fe_invert(&z2, &z2);
	fe_mul(&x2, &x2, &z2);
	fe_canon(&x2);
	fe_tobytes(out, &x2);
}
