/* Internal exact decimal decomposition for stdio float formatting. */
#ifndef LIBC_DTOA_H
#define LIBC_DTOA_H

#define LIBC_DTOA_MAX 1200

struct libc_dtoa {
	char digits[LIBC_DTOA_MAX];
	int ndigits;
	int point;      /* digits to the left of the decimal point */
	int neg;        /* sign bit, including negative zero */
};

/* Returns 0 for finite, 1 for infinity and 2 for NaN. */
int libc_dtoa(double value, struct libc_dtoa *out);

#endif
