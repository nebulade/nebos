/* Minimal float.h for the ring-3 libc (i386 double). */
#ifndef LIBC_FLOAT_H
#define LIBC_FLOAT_H

#define DBL_MANT_DIG     53
#define DBL_DIG          15
#define DBL_MIN_EXP      (-1021)
#define DBL_MAX_EXP      1024
#define DBL_MIN_10_EXP   (-307)
#define DBL_MAX_10_EXP   308
#define DBL_MAX          1.7976931348623157e+308
#define DBL_MIN          2.2250738585072014e-308
#define DBL_EPSILON      2.2204460492503131e-16
#define DBL_TRUE_MIN     4.9406564584124654e-324

#define FLT_MANT_DIG     24
#define FLT_DIG          6
#define FLT_MIN_EXP      (-125)
#define FLT_MAX_EXP      128
#define FLT_MIN_10_EXP   (-37)
#define FLT_MAX_10_EXP   38
#define FLT_MAX          3.402823466e+38F
#define FLT_MIN          1.175494351e-38F
#define FLT_EPSILON      1.192092896e-07F

#endif
