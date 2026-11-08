/* Minimal C99 math interface for the ring-3 libc. */
#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#include <float.h>

#define INFINITY  (__builtin_inf())
#define NAN       (__builtin_nan(""))
#define HUGE_VAL  (__builtin_huge_val())

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#define fpclassify(x) \
	(__builtin_isnan(x) ? 0 : __builtin_isinf(x) ? 1 : \
	 (x) == 0 ? 2 : __builtin_isnormal(x) ? 4 : 3)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define isnormal(x)  __builtin_isnormal(x)

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double sqrt(double x);

double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double fmin(double x, double y);
double fmax(double x, double y);
double copysign(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);

#endif
