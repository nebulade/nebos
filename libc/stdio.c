/* Formatted output for the ring-3 libc.
 *
 * The formatting core is syscall-free; stdio_file.c supplies console streams
 * and read-only whole-file FILE objects.  Float formatting uses the exact
 * decimal decomposition in dtoa.c, then rounds at the requested precision. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "dtoa.h"

#define F_LEFT  0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_HASH  0x08
#define F_ZERO  0x10

struct out {
	char *buf;
	size_t cap;
	size_t len;
};

static void out_char(struct out *o, char c)
{
	if (o->buf && o->len < o->cap)
		o->buf[o->len] = c;
	o->len++;
}

static void out_mem(struct out *o, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		out_char(o, s[i]);
}

static int utoa_base(unsigned long long v, unsigned int base, int upper,
		     char *buf)
{
	static const char low[] = "0123456789abcdef";
	static const char up[] = "0123456789ABCDEF";
	const char *digits = upper ? up : low;
	char tmp[66];
	int n = 0;
	int i;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	for (i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	buf[n] = '\0';
	return n;
}

static void emit_padded(struct out *o, const char *s, int len, int width,
			int flags, int numeric)
{
	int pad = width > len ? width - len : 0;
	int i;
	int sign = 0;

	if (numeric && len > 0 && (s[0] == '-' || s[0] == '+' || s[0] == ' '))
		sign = 1;

	if (!(flags & F_LEFT)) {
		if ((flags & F_ZERO) && numeric) {
			for (i = 0; i < sign; i++)
				out_char(o, s[i]);
			for (i = 0; i < pad; i++)
				out_char(o, '0');
			out_mem(o, s + sign, (size_t)(len - sign));
			return;
		}
		for (i = 0; i < pad; i++)
			out_char(o, ' ');
	}
	out_mem(o, s, (size_t)len);
	if (flags & F_LEFT) {
		for (i = 0; i < pad; i++)
			out_char(o, ' ');
	}
}

/* Round a digit string to `keep` digits.  Returns the new length and sets
 * *carry when a leading '1' was inserted. */
static int round_digits(char *d, int n, int keep, int *carry)
{
	int next;
	int up = 0;
	int i;

	*carry = 0;
	if (keep >= n)
		return n;
	if (keep < 0)
		keep = 0;

	next = d[keep];
	if (next > '5') {
		up = 1;
	} else if (next == '5') {
		int nonzero = 0;

		for (i = keep + 1; i < n; i++) {
			if (d[i] != '0') {
				nonzero = 1;
				break;
			}
		}
		if (nonzero)
			up = 1;
		else if (keep > 0 && ((d[keep - 1] - '0') & 1))
			up = 1;
	}

	n = keep;
	if (!up)
		return n;
	i = n - 1;
	while (i >= 0 && d[i] == '9') {
		d[i] = '0';
		i--;
	}
	if (i >= 0) {
		d[i]++;
	} else {
		memmove(d + 1, d, (size_t)n);
		d[0] = '1';
		n++;
		*carry = 1;
	}
	return n;
}

static void prepend_zeros(char *d, int *n, int count)
{
	if (count <= 0)
		return;
	if (*n + count > LIBC_DTOA_MAX)
		count = LIBC_DTOA_MAX - *n;
	if (count <= 0)
		return;
	memmove(d + count, d, (size_t)*n);
	memset(d, '0', (size_t)count);
	*n += count;
}

static void format_fixed(const struct libc_dtoa *dt, int prec, int hash,
			 char *body, int *blen)
{
	char d[LIBC_DTOA_MAX];
	int n = dt->ndigits;
	int point = dt->point;
	int keep;
	int carry = 0;

	if (prec > LIBC_DTOA_MAX - 8)
		prec = LIBC_DTOA_MAX - 8;
	memcpy(d, dt->digits, (size_t)n);

	if (point < 0) {
		prepend_zeros(d, &n, -point);
		point = 0;
	}
	if (point > n) {
		if (point + prec > LIBC_DTOA_MAX)
			point = LIBC_DTOA_MAX - prec - 1;
		while (n < point)
			d[n++] = '0';
	}
	keep = point + prec;
	if (keep > LIBC_DTOA_MAX - 2)
		keep = LIBC_DTOA_MAX - 2;
	while (n < keep)
		d[n++] = '0';
	if (keep < n)
		n = round_digits(d, n, keep, &carry);
	if (carry)
		point++;

	{
		int i = 0;

		if (point == 0) {
			body[i++] = '0';
		} else {
			int j;

			for (j = 0; j < point; j++)
				body[i++] = (j < n) ? d[j] : '0';
		}
		if (prec > 0 || hash) {
			body[i++] = '.';
			for (int j = 0; j < prec; j++)
				body[i++] = (point + j < n) ? d[point + j] : '0';
		}
		body[i] = '\0';
		*blen = i;
	}
}

static void format_exp(const struct libc_dtoa *dt, int prec, int hash,
		       char *body, int *blen)
{
	char d[LIBC_DTOA_MAX];
	int n = dt->ndigits;
	int point = dt->point;
	int keep;
	int carry;
	int exp10 = point - 1;
	int i;
	char *p = body;

	if (prec < 0)
		prec = 0;
	if (prec > 900)
		prec = 900;
	keep = prec + 1;
	if (keep > LIBC_DTOA_MAX - 2)
		keep = LIBC_DTOA_MAX - 2;
	memcpy(d, dt->digits, (size_t)n);
	while (n < keep)
		d[n++] = '0';
	if (keep < n)
		n = round_digits(d, n, keep, &carry);
	else
		carry = 0;
	if (carry) {
		exp10++;
		n = keep;
	}

	*p++ = d[0];
	if (prec > 0 || hash) {
		*p++ = '.';
		for (i = 1; i <= prec; i++)
			*p++ = (i < n) ? d[i] : '0';
	}
	*p++ = 'e';
	{
		int e = exp10;
		if (e < 0) {
			*p++ = '-';
			e = -e;
		} else {
			*p++ = '+';
		}
		if (e >= 100) {
			*p++ = (char)('0' + (e / 100) % 10);
			*p++ = (char)('0' + (e / 10) % 10);
			*p++ = (char)('0' + e % 10);
		} else {
			*p++ = (char)('0' + (e / 10) % 10);
			*p++ = (char)('0' + e % 10);
		}
	}
	*p = '\0';
	*blen = (int)(p - body);
}

static void strip_float_zeros(char *body, int *len)
{
	char *e = strchr(body, 'e');
	char *dot = strchr(body, '.');
	char *end = e ? e : body + *len;

	if (!dot)
		return;
	while (end > dot && end[-1] == '0')
		end--;
	if (end > body && end[-1] == '.')
		end--;
	if (e) {
		int tail = (int)(*len - (e - body));

		if (end != e)
			memmove(end, e, (size_t)tail + 1);
		*len = (int)(end - body) + tail;
	} else {
		*len = (int)(end - body);
		body[*len] = '\0';
	}
}

static int significant_exp10(const struct libc_dtoa *dt, int prec)
{
	char d[LIBC_DTOA_MAX];
	int n = dt->ndigits;
	int carry;

	if (prec < 1)
		prec = 1;
	if (n > prec) {
		memcpy(d, dt->digits, (size_t)n);
		n = round_digits(d, n, prec, &carry);
		if (carry)
			return dt->point;
	}
	return dt->point - 1;
}

static void format_double(struct out *o, double value, char conv,
			  int prec, int flags, int width)
{
	struct libc_dtoa dt;
	char body[LIBC_DTOA_MAX + 96];
	char num[LIBC_DTOA_MAX + 100];
	int blen = 0;
	int n;
	int sign;
	int cls;
	int body_start;

	cls = libc_dtoa(value, &dt);
	if (cls == 1) {
		blen = 3;
		memcpy(body, "inf", 4);
		sign = dt.neg;
	} else if (cls == 2) {
		blen = 3;
		memcpy(body, "nan", 4);
		sign = dt.neg;
	} else {
		sign = dt.neg;
		if (prec < 0)
			prec = 6;
		if (prec > 900)
			prec = 900;
		if (conv == 'f' || conv == 'F') {
			format_fixed(&dt, prec, (flags & F_HASH) != 0, body, &blen);
		} else if (conv == 'e' || conv == 'E') {
			format_exp(&dt, prec, (flags & F_HASH) != 0, body, &blen);
		} else {
			int p = prec;
			int exp10;

			if (p == 0)
				p = 1;
			exp10 = significant_exp10(&dt, p);
			if (exp10 < -4 || exp10 >= p) {
				format_exp(&dt, p - 1, (flags & F_HASH) != 0,
					   body, &blen);
			} else {
				int fp = p - (exp10 + 1);
				if (fp < 0)
					fp = 0;
				format_fixed(&dt, fp, (flags & F_HASH) != 0,
					     body, &blen);
			}
			if (!(flags & F_HASH))
				strip_float_zeros(body, &blen);
		}
	}

	body_start = 0;
	if (sign) {
		num[0] = '-';
		body_start = 1;
	} else if (flags & F_PLUS) {
		num[0] = '+';
		body_start = 1;
	} else if (flags & F_SPACE) {
		num[0] = ' ';
		body_start = 1;
	}
	memcpy(num + body_start, body, (size_t)blen + 1);
	n = body_start + blen;

	/* Uppercase E/Inf/NaN for the uppercase conversions. */
	if (conv == 'E' || conv == 'F' || conv == 'G') {
		for (int i = body_start; i < n; i++) {
			if (num[i] == 'e')
				num[i] = 'E';
			else if (num[i] == 'n')
				num[i] = 'N';
			else if (num[i] == 'i')
				num[i] = 'I';
		}
	}
	emit_padded(o, num, n, width, flags, 1);
}

int vsnprintf(char *buf, size_t size, const char *format, va_list ap)
{
	struct out o;
	const char *p;

	o.buf = (buf && size) ? buf : 0;
	o.cap = (buf && size) ? size : 0;
	o.len = 0;

	for (p = format; *p; p++) {
		int flags = 0;
		int width = 0;
		int prec = -1;
		int len = -1;
		char conv;

		if (*p != '%') {
			out_char(&o, *p);
			continue;
		}
		p++;
		for (;;) {
			if (*p == '-') flags |= F_LEFT;
			else if (*p == '+') flags |= F_PLUS;
			else if (*p == ' ') flags |= F_SPACE;
			else if (*p == '#') flags |= F_HASH;
			else if (*p == '0') flags |= F_ZERO;
			else break;
			p++;
		}
		if (*p == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			p++;
		} else {
			while (*p >= '0' && *p <= '9') {
				width = width * 10 + (*p - '0');
				p++;
			}
		}
		if (*p == '.') {
			p++;
			if (*p == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				p++;
			} else {
				prec = 0;
				while (*p >= '0' && *p <= '9') {
					prec = prec * 10 + (*p - '0');
					p++;
				}
			}
		}
		if (*p == 'h') {
			p++;
			if (*p == 'h') {
				len = 0;
				p++;
			} else {
				len = 1;
			}
		} else if (*p == 'l') {
			p++;
			if (*p == 'l') {
				len = 3;
				p++;
			} else {
				len = 2;
			}
		} else if (*p == 'z') {
			len = 4;
			p++;
		} else if (*p == 't') {
			len = 5;
			p++;
		} else if (*p == 'j') {
			len = 6;
			p++;
		} else if (*p == 'L') {
			len = 7;
			p++;
		}

		conv = *p;
		if (!conv)
			break;

		if (conv == '%') {
			out_char(&o, '%');
			continue;
		}
		if (conv == 'c') {
			char c = (char)va_arg(ap, int);
			char s[2];
			s[0] = c;
			s[1] = '\0';
			emit_padded(&o, s, 1, width, flags & ~F_ZERO, 0);
			continue;
		}
		if (conv == 's') {
			const char *s = va_arg(ap, const char *);
			int n = 0;
			if (!s)
				s = "(null)";
			while (s[n] && (prec < 0 || n < prec))
				n++;
			emit_padded(&o, s, n, width, flags & ~F_ZERO, 0);
			continue;
		}
		if (conv == 'n') {
			int *ip = va_arg(ap, int *);

			/* Do not write during vfprintf's sizing pass. */
			if (ip && o.buf)
				*ip = (int)o.len;
			continue;
		}
		if (conv == 'f' || conv == 'F' || conv == 'e' ||
		    conv == 'E' || conv == 'g' || conv == 'G') {
			format_double(&o, va_arg(ap, double), conv, prec, flags,
				      width);
			continue;
		}

		{
			int base = 10;
			int upper = 0;
			int is_signed = 0;
			int is_ptr = 0;
			long long sv = 0;
			unsigned long long uv = 0;
			char digits[80];
			char body[96];
			int nd;
			int bn = 0;
			int prefix = 0;
			int neg = 0;

			if (conv == 'd' || conv == 'i') {
				is_signed = 1;
			} else if (conv == 'u') {
				base = 10;
			} else if (conv == 'o') {
				base = 8;
			} else if (conv == 'x') {
				base = 16;
			} else if (conv == 'X') {
				base = 16;
				upper = 1;
			} else if (conv == 'p') {
				base = 16;
				is_ptr = 1;
			} else {
				out_char(&o, '%');
				out_char(&o, conv);
				continue;
			}

			if (is_ptr) {
				uv = (unsigned long)va_arg(ap, void *);
			} else if (is_signed) {
				switch (len) {
				case 0: sv = (signed char)va_arg(ap, int); break;
				case 1: sv = (short)va_arg(ap, int); break;
				case 2: sv = va_arg(ap, long); break;
				case 3: sv = va_arg(ap, long long); break;
				case 4: sv = (long)va_arg(ap, size_t); break;
				case 5: sv = (long)va_arg(ap, ptrdiff_t); break;
				case 6: sv = va_arg(ap, long long); break;
				default: sv = va_arg(ap, int); break;
				}
				if (sv < 0) {
					neg = 1;
					uv = (unsigned long long)(-(sv + 1)) + 1ULL;
				} else {
					uv = (unsigned long long)sv;
				}
			} else {
				switch (len) {
				case 0: uv = (unsigned char)va_arg(ap, unsigned int); break;
				case 1: uv = (unsigned short)va_arg(ap, unsigned int); break;
				case 2: uv = va_arg(ap, unsigned long); break;
				case 3: uv = va_arg(ap, unsigned long long); break;
				case 4: uv = va_arg(ap, size_t); break;
				case 5: uv = (unsigned long)va_arg(ap, ptrdiff_t); break;
				case 6: uv = va_arg(ap, unsigned long long); break;
				default: uv = va_arg(ap, unsigned int); break;
				}
			}

			if (prec > 64)
				prec = 64;
			if (is_ptr) {
				prefix = 2;
			} else if ((flags & F_HASH) && base == 16 && uv != 0) {
				prefix = 2;
			} else if ((flags & F_HASH) && base == 8) {
				prefix = 1;
			}

			nd = utoa_base(uv, (unsigned int)base, upper, digits);
			if (prec == 0 && uv == 0) {
				nd = 0;
				digits[0] = '\0';
			}
			while (nd < prec) {
				memmove(digits + 1, digits, (size_t)nd + 1);
				digits[0] = '0';
				nd++;
			}
			if (neg) {
				body[bn++] = '-';
			} else if (flags & F_PLUS) {
				body[bn++] = '+';
			} else if (flags & F_SPACE) {
				body[bn++] = ' ';
			}
			if (prefix == 2) {
				body[bn++] = '0';
				body[bn++] = upper ? 'X' : 'x';
			} else if (prefix == 1 && (nd == 0 || digits[0] != '0')) {
				body[bn++] = '0';
			}
			memcpy(body + bn, digits, (size_t)nd);
			bn += nd;
			body[bn] = '\0';
			emit_padded(&o, body, bn, width, flags, 1);
		}
	}

	if (o.cap) {
		size_t end = o.len < o.cap ? o.len : o.cap - 1;
		o.buf[end] = '\0';
	}
	if (o.len > INT_MAX)
		return -1;
	return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(buf, size, format, ap);
	va_end(ap);
	return n;
}

int vsprintf(char *buf, const char *format, va_list ap)
{
	return vsnprintf(buf, (size_t)-1, format, ap);
}

int sprintf(char *buf, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsprintf(buf, format, ap);
	va_end(ap);
	return n;
}

int vfprintf(FILE *stream, const char *format, va_list ap)
{
	va_list cp;
	int n;
	char *buf;

	va_copy(cp, ap);
	n = vsnprintf(0, 0, format, cp);
	va_end(cp);
	if (n < 0)
		return n;
	buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		errno = ENOMEM;
		return -1;
	}
	vsnprintf(buf, (size_t)n + 1, format, ap);
	fwrite(buf, 1, (size_t)n, stream);
	free(buf);
	return n;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vfprintf(stream, format, ap);
	va_end(ap);
	return n;
}

int vprintf(const char *format, va_list ap)
{
	return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vfprintf(stdout, format, ap);
	va_end(ap);
	return n;
}

int puts(const char *s)
{
	if (fputs(s, stdout) < 0)
		return EOF;
	if (fputc('\n', stdout) < 0)
		return EOF;
	return 0;
}

int putchar(int c)
{
	return fputc(c, stdout);
}

void perror(const char *s)
{
	if (s && *s)
		fprintf(stderr, "%s: %s\n", s, strerror(errno));
	else
		fprintf(stderr, "%s\n", strerror(errno));
}
