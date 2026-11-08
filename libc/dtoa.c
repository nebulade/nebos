/* Exact binary-double to decimal decomposition.
 *
 * A finite double is m * 2^e.  For e >= 0 it is an integer; for e < 0 we
 * compute m * 5^(-e) / 10^(-e), so the decimal expansion is exact.  The
 * result is a digit string plus the decimal-point position.  stdio.c does
 * precision rounding and the %f/%e/%g layout from this representation. */

#include <string.h>
#include "dtoa.h"

#define DTOA_LIMBS 128

static void big_zero(unsigned int *a, int *n)
{
	int i;

	for (i = 0; i < DTOA_LIMBS; i++)
		a[i] = 0;
	*n = 0;
}

static void big_set_u64(unsigned int *a, int *n, unsigned long long v)
{
	big_zero(a, n);
	a[0] = (unsigned int)v;
	a[1] = (unsigned int)(v >> 32);
	*n = a[1] ? 2 : (a[0] ? 1 : 0);
}

static void big_shl(unsigned int *a, int *n, unsigned int bits)
{
	unsigned int words = bits / 32u;
	unsigned int shift = bits % 32u;
	int i;

	if (words) {
		for (i = *n - 1; i >= 0; i--)
			a[i + words] = a[i];
		for (i = 0; i < (int)words; i++)
			a[i] = 0;
		*n += (int)words;
	}
	if (shift) {
		unsigned long long carry = 0;

		for (i = 0; i < *n; i++) {
			unsigned long long v =
				((unsigned long long)a[i] << shift) | carry;

			a[i] = (unsigned int)v;
			carry = v >> 32;
		}
		if (carry && *n < DTOA_LIMBS)
			a[(*n)++] = (unsigned int)carry;
	}
}

static void big_mul5(unsigned int *a, int *n)
{
	unsigned long long carry = 0;
	int i;

	for (i = 0; i < *n; i++) {
		unsigned long long v = (unsigned long long)a[i] * 5u + carry;
		a[i] = (unsigned int)v;
		carry = v >> 32;
	}
	if (carry && *n < DTOA_LIMBS)
		a[(*n)++] = (unsigned int)carry;
}

/* Divide by 10 in place; returns the remainder. */
static unsigned int big_div10(unsigned int *a, int *n)
{
	unsigned long long rem = 0;
	int i;

	for (i = *n - 1; i >= 0; i--) {
		unsigned long long cur = (rem << 32) | a[i];
		a[i] = (unsigned int)(cur / 10u);
		rem = cur % 10u;
	}
	while (*n > 0 && a[*n - 1] == 0)
		(*n)--;
	return (unsigned int)rem;
}

static int big_to_digits(unsigned int *a, int *n, char *digits, int *point)
{
	char rev[LIBC_DTOA_MAX];
	int len = 0;
	int i;

	while (*n > 0) {
		unsigned int d = big_div10(a, n);
		if (len >= LIBC_DTOA_MAX)
			break;
		rev[len++] = (char)('0' + d);
	}
	if (len == 0) {
		digits[0] = '0';
		*point = 1;
		return 1;
	}
	for (i = 0; i < len; i++)
		digits[i] = rev[len - 1 - i];
	*point = len;
	return len;
}

int libc_dtoa(double value, struct libc_dtoa *out)
{
	union {
		double d;
		unsigned long long u;
	} v;
	unsigned long long bits;
	unsigned int exp;
	unsigned long long mant;
	int e2;
	unsigned int a[DTOA_LIMBS];
	int n;
	int len;

	v.d = value;
	bits = v.u;
	out->neg = (int)(bits >> 63);
	exp = (unsigned int)((bits >> 52) & 0x7ffu);
	mant = bits & 0x000fffffffffffffULL;

	if (exp == 0x7ffu)
		return mant ? 2 : 1;

	if (exp == 0 && mant == 0) {
		out->digits[0] = '0';
		out->ndigits = 1;
		out->point = 1;
		return 0;
	}

	if (exp == 0) {
		e2 = -1074;
	} else {
		mant |= 1ULL << 52;
		e2 = (int)exp - 1023 - 52;
	}

	big_set_u64(a, &n, mant);
	if (e2 >= 0) {
		big_shl(a, &n, (unsigned int)e2);
		len = big_to_digits(a, &n, out->digits, &out->point);
	} else {
		int k = -e2;
		while (k-- > 0)
			big_mul5(a, &n);
		len = big_to_digits(a, &n, out->digits, &out->point);
		out->point = len + e2;
	}
	out->ndigits = len;
	return 0;
}
