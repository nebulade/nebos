/* Compiler runtime helpers used by MuJS and printf on i386.
 *
 * GCC emits these for 64-bit division/modulo when no libgcc is linked. */

typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod(u64 n, u64 d, u64 *rem)
{
	u64 q = 0;
	int shift = 0;
	u64 dd = d;

	if (d == 0) {
		if (rem)
			*rem = n;
		return 0;
	}
	if (n < d) {
		if (rem)
			*rem = n;
		return 0;
	}

	while (!(dd & 0x8000000000000000ULL)) {
		if (dd > n)
			break;
		dd <<= 1;
		shift++;
	}
	for (;;) {
		if (n >= dd) {
			n -= dd;
			q |= 1ULL << shift;
		}
		if (shift == 0)
			break;
		dd >>= 1;
		shift--;
	}
	if (rem)
		*rem = n;
	return q;
}

u64 __udivmoddi4(u64 n, u64 d, u64 *rem)
{
	return udivmod(n, d, rem);
}

u64 __udivdi3(u64 n, u64 d)
{
	return udivmod(n, d, 0);
}

u64 __umoddi3(u64 n, u64 d)
{
	u64 r;

	udivmod(n, d, &r);
	return r;
}

static u64 absu(s64 v)
{
	return v < 0 ? (u64)(-(v + 1)) + 1 : (u64)v;
}

s64 __divdi3(s64 n, s64 d)
{
	u64 q = udivmod(absu(n), absu(d), 0);

	if ((n < 0) != (d < 0))
		return -(s64)q;
	return (s64)q;
}

s64 __moddi3(s64 n, s64 d)
{
	u64 r;

	udivmod(absu(n), absu(d), &r);
	return n < 0 ? -(s64)r : (s64)r;
}

s64 __divmoddi4(s64 n, s64 d, s64 *rem)
{
	u64 r;
	u64 q = udivmod(absu(n), absu(d), &r);

	if (rem)
		*rem = n < 0 ? -(s64)r : (s64)r;
	if ((n < 0) != (d < 0))
		return -(s64)q;
	return (s64)q;
}

/* A few shifts that older/less clever i386 code paths may still call. */
u64 __ashldi3(u64 v, int n)
{
	return n >= 64 ? 0 : v << n;
}

u64 __lshrdi3(u64 v, int n)
{
	return n >= 64 ? 0 : v >> n;
}

s64 __ashrdi3(s64 v, int n)
{
	if (n >= 64)
		return v < 0 ? -1 : 0;
	return v >> n;
}
