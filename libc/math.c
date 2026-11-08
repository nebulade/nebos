/* x87-backed math for the ring-3 libc.
 *
 * The target is i586 with -mno-sse, so double arithmetic is x87 anyway.  The
 * transcendental routines use the x87 instructions with an explicit range
 * reduction for the trigonometric functions.  Accuracy is a double-rounding
 * compromise, not a full fdlibm port. */

#include <math.h>
#include <float.h>

#define PI      3.14159265358979323846
#define LOG2E   1.44269504088896340736
#define LN2     0.69314718055994530942

static double round_mode(double x, unsigned int mode)
{
	unsigned short cw, old;
	double r;

	if (__builtin_isnan(x) || __builtin_isinf(x) || x == 0.0)
		return x;
	__asm__ volatile ("fnstcw %0" : "=m"(cw));
	old = cw;
	cw = (unsigned short)((cw & ~0x0C00u) | (mode << 10));
	__asm__ volatile ("fldcw %0" : : "m"(cw));
	__asm__ volatile ("fldl %1; frndint; fstpl %0" : "=m"(r) : "m"(x));
	__asm__ volatile ("fldcw %0" : : "m"(old));
	return r;
}

double fabs(double x)
{
	union {
		double d;
		unsigned long long u;
	} v;

	v.d = x;
	v.u &= 0x7fffffffffffffffULL;
	return v.d;
}

double floor(double x)
{
	return round_mode(x, 1);
}

double ceil(double x)
{
	return round_mode(x, 2);
}

double trunc(double x)
{
	return round_mode(x, 3);
}

double round(double x)
{
	if (__builtin_isnan(x) || __builtin_isinf(x))
		return x;
	/* Above 2^52 every double is already an integer. */
	if (fabs(x) >= 4503599627370496.0)
		return x;
	if (x >= 0.0)
		return floor(x + 0.5);
	return ceil(x - 0.5);
}

double sqrt(double x)
{
	double r;

	__asm__ volatile ("fldl %1; fsqrt; fstpl %0" : "=m"(r) : "m"(x));
	return r;
}

double fmod(double x, double y)
{
	double r;

	__asm__ volatile (
		"fldl %2\n\t"
		"fldl %1\n\t"
		"1:\n\t"
		"fprem\n\t"
		"fnstsw %%ax\n\t"
		"testw $0x400, %%ax\n\t"
		"jnz 1b\n\t"
		"fstp %%st(1)\n\t"
		"fstpl %0"
		: "=m"(r) : "m"(x), "m"(y) : "ax");
	return r;
}

double log(double x)
{
	double r;

	__asm__ volatile ("fldln2; fldl %1; fyl2x; fstpl %0"
			  : "=m"(r) : "m"(x));
	return r;
}

double log10(double x)
{
	double r;

	__asm__ volatile ("fldlg2; fldl %1; fyl2x; fstpl %0"
			  : "=m"(r) : "m"(x));
	return r;
}

double ldexp(double x, int exp)
{
	double r;

	__asm__ volatile (
		"fildl %1\n\t"
		"fldl %2\n\t"
		"fscale\n\t"
		"fstp %%st(1)\n\t"
		"fstpl %0"
		: "=m"(r) : "m"(exp), "m"(x));
	return r;
}

double exp(double x)
{
	double y, n, f, p;

	if (__builtin_isnan(x))
		return x;
	if (x > 709.782712893384)
		return INFINITY;
	if (x < -745.133219101941)
		return 0.0;

	y = x * LOG2E;
	n = floor(y);
	f = y - n;
	__asm__ volatile ("fldl %1; f2xm1; fld1; faddp; fstpl %0"
			  : "=m"(p) : "m"(f));
	return ldexp(p, (int)n);
}

double pow(double x, double y)
{
	int neg = 0;
	double r;

	if (y == 0.0)
		return 1.0;
	if (x == 1.0)
		return 1.0;
	if (__builtin_isnan(x) || __builtin_isnan(y))
		return NAN;
	if (x == 0.0) {
		if (y < 0.0)
			return INFINITY;
		return 0.0;
	}
	if (x < 0.0) {
		double yi = floor(y);

		if (yi != y)
			return NAN;
		x = -x;
		if (fmod(yi, 2.0) != 0.0)
			neg = 1;
	}

	/* Small integer powers are common in JavaScript and stay exact. */
	if (y == floor(y) && y >= -64.0 && y <= 64.0) {
		int n = (int)y;
		int e = n < 0 ? -n : n;

		r = 1.0;
		while (e) {
			if (e & 1)
				r *= x;
			x *= x;
			e >>= 1;
		}
		if (n < 0)
			r = 1.0 / r;
	} else {
		r = exp(y * log(x));
	}
	return neg ? -r : r;
}

double sin(double x)
{
	double r;

	/* The x87 reduction uses a 66-bit pi internally; pre-reducing with a
	 * double 2*pi would lose more phase for large arguments. */
	__asm__ volatile ("fldl %1; fsin; fstpl %0" : "=m"(r) : "m"(x));
	return r;
}

double cos(double x)
{
	double r;

	__asm__ volatile ("fldl %1; fcos; fstpl %0" : "=m"(r) : "m"(x));
	return r;
}

double tan(double x)
{
	double r;

	__asm__ volatile ("fldl %1; fptan; fstp %%st(0); fstpl %0"
			  : "=m"(r) : "m"(x));
	return r;
}

double atan(double x)
{
	double r;

	__asm__ volatile ("fldl %1; fld1; fpatan; fstpl %0"
			  : "=m"(r) : "m"(x));
	return r;
}

double atan2(double y, double x)
{
	double r;

	__asm__ volatile ("fldl %1; fldl %2; fpatan; fstpl %0"
			  : "=m"(r) : "m"(y), "m"(x));
	return r;
}

double asin(double x)
{
	if (x > 1.0 || x < -1.0)
		return NAN;
	return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
	if (x > 1.0 || x < -1.0)
		return NAN;
	return atan2(sqrt(1.0 - x * x), x);
}

double fmin(double x, double y)
{
	if (__builtin_isnan(x))
		return y;
	if (__builtin_isnan(y))
		return x;
	return x < y ? x : y;
}

double fmax(double x, double y)
{
	if (__builtin_isnan(x))
		return y;
	if (__builtin_isnan(y))
		return x;
	return x > y ? x : y;
}

double copysign(double x, double y)
{
	union {
		double d;
		unsigned long long u;
	} a, b;

	a.d = x;
	b.d = y;
	a.u = (a.u & 0x7fffffffffffffffULL) |
	      (b.u & 0x8000000000000000ULL);
	return a.d;
}

double frexp(double x, int *exp)
{
	union {
		double d;
		unsigned long long u;
	} v;
	unsigned long long bits;
	unsigned int e;
	int extra = 0;

	if (__builtin_isnan(x) || __builtin_isinf(x) || x == 0.0) {
		if (exp)
			*exp = 0;
		return x;
	}
	if (fabs(x) < DBL_MIN) {
		x *= 18446744073709551616.0; /* 2^64 */
		extra = 64;
	}
	v.d = x;
	bits = v.u;
	e = (unsigned int)((bits >> 52) & 0x7ffu);
	if (exp)
		*exp = (int)e - 1022 - extra;
	bits = (bits & 0x800fffffffffffffULL) | (1022ULL << 52);
	v.u = bits;
	return v.d;
}

double modf(double x, double *iptr)
{
	double ip = trunc(x);

	if (iptr)
		*iptr = ip;
	return x - ip;
}
