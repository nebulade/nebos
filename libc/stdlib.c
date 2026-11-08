/* Minimal freestanding stdlib support for ring-3 commands. */

#include <stddef.h>
#include <stdlib.h>
#include <ucmd_lib.h>

int abs(int value)
{
	return value < 0 ? -value : value;
}

long strtol(const char *str, char **endptr, int base)
{
	const char *p = str;
	int negative = 0;
	int any = 0;
	unsigned long value = 0;

	if (!str) {
		if (endptr)
			*endptr = (char *)str;
		return 0;
	}

	while (*p == ' ' || *p == '\t' || *p == '\n' ||
	       *p == '\v' || *p == '\f' || *p == '\r')
		p++;

	if (*p == '+' || *p == '-') {
		negative = (*p == '-');
		p++;
	}

	if ((base == 0 || base == 16) && p[0] == '0' &&
	    (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	} else if (base == 0) {
		base = (p[0] == '0') ? 8 : 10;
	}

	if (base < 2 || base > 36) {
		if (endptr)
			*endptr = (char *)str;
		return 0;
	}

	for (;;) {
		int digit;

		if (*p >= '0' && *p <= '9')
			digit = *p - '0';
		else if (*p >= 'a' && *p <= 'z')
			digit = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'Z')
			digit = *p - 'A' + 10;
		else
			break;
		if (digit >= base)
			break;
		value = value * (unsigned long)base + (unsigned long)digit;
		any = 1;
		p++;
	}

	if (!any) {
		if (endptr)
			*endptr = (char *)str;
		return 0;
	}
	if (endptr)
		*endptr = (char *)p;
	return negative ? -(long)value : (long)value;
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n)
{
	while (n--) {
		unsigned char t = *a;

		*a++ = *b;
		*b++ = t;
	}
}

/* Sift element `start` down in a max heap. */
static void sift_down(unsigned char *base, size_t start, size_t end,
		      size_t size, int (*compar)(const void *, const void *))
{
	size_t root = start;

	while (root * 2 + 1 <= end) {
		size_t child = root * 2 + 1;

		if (child + 1 <= end &&
		    compar(base + child * size, base + (child + 1) * size) < 0)
			child++;
		if (compar(base + root * size, base + child * size) < 0) {
			swap_bytes(base + root * size, base + child * size, size);
			root = child;
		} else {
			return;
		}
	}
}

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *))
{
	unsigned char *p = (unsigned char *)base;
	size_t start;
	size_t end;

	if (!p || !compar || nmemb < 2 || size == 0)
		return;

	/* Build the max heap. */
	start = (nmemb - 2) / 2;
	for (;;) {
		sift_down(p, start, nmemb - 1, size, compar);
		if (start == 0)
			break;
		start--;
	}

	/* Repeatedly move the maximum to the end and shrink the heap. */
	end = nmemb - 1;
	while (end > 0) {
		swap_bytes(p, p + end * size, size);
		end--;
		sift_down(p, 0, end, size, compar);
	}
}

void exit(int status)
{
	u_exit(status);
	for (;;)
		;
}

void abort(void)
{
	u_puts("abort\n");
	u_exit(1);
	for (;;)
		;
}

void __assert_fail(const char *expr, const char *file, unsigned int line)
{
	u_puts("assert failed: ");
	u_puts(expr ? expr : "?");
	u_puts(" (");
	u_puts(file ? file : "?");
	u_puts(":");
	u_putdec(line);
	u_puts(")\n");
	u_exit(1);
	for (;;)
		;
}
