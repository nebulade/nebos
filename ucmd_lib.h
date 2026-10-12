/* Tiny userland shim for ring-3 commands: syscall wrappers + string output.
 * Freestanding (no libc); include "syscall.h" for the syscall numbers. */
#ifndef UCMD_LIB_H
#define UCMD_LIB_H

#include "syscall.h"

static inline int u_syscall1(int num, int a1)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1)
			  : "memory");
	return ret;
}

static inline int u_syscall0(int num)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num)
			  : "memory");
	return ret;
}

static inline int u_syscall2(int num, int a1, int a2)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1), "c"(a2)
			  : "memory");
	return ret;
}

static inline int u_syscall3(int num, int a1, int a2, int a3)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1), "c"(a2), "d"(a3)
			  : "memory");
	return ret;
}

static inline void u_putc(char c)
{
	char b[2];

	b[0] = c;
	b[1] = '\0';
	u_syscall1(SYS_PUTSTR, (int)b);
}

static inline void u_puts(const char *s)
{
	u_syscall1(SYS_PUTSTR, (int)s);
}

static inline void u_putdec(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;

	*p = '\0';
	if (n == 0) {
		u_putc('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	u_puts(p);
}

static inline void u_puthex(unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[11];
	int i;

	for (i = 0; i < 8; i++)
		buf[i] = hex[(n >> (28 - i * 4)) & 0xF];
	buf[8] = '\0';
	u_puts(buf);
}

/* "a.b.c.d" (strict, 4 parts, 0-255) -> 0, else -1. */
static inline int u_parse_ip(const char *s, unsigned char ip[4])
{
	unsigned int i, val = 0, nparts = 0;
	int have_digit = 0;

	for (i = 0; ; i++) {
		char c = s[i];

		if (c >= '0' && c <= '9') {
			val = val * 10 + (unsigned int)(c - '0');
			if (val > 255)
				return -1;
			have_digit = 1;
		} else if (c == '.' || c == '\0') {
			if (!have_digit || nparts >= 4)
				return -1;
			ip[nparts++] = (unsigned char)val;
			val = 0;
			have_digit = 0;
			if (c == '\0')
				break;
		} else {
			return -1;
		}
	}
	return (nparts == 4) ? 0 : -1;
}

static inline void u_put_ip(const unsigned char ip[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (i)
			u_putc('.');
		u_putdec(ip[i]);
	}
}

static inline void u_put_hex2(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";

	u_putc(hex[(v >> 4) & 0xF]);
	u_putc(hex[v & 0xF]);
}

static inline void u_put_mac(const unsigned char mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		if (i)
			u_putc(':');
		u_put_hex2(mac[i]);
	}
}

static inline unsigned int u_ticks_ms(void)
{
	return (unsigned int)u_syscall0(SYS_GET_TICKS_MS);
}

static inline void u_sleep_ms(unsigned int ms)
{
	u_syscall1(SYS_SLEEP_MS, (int)ms);
}

static inline void u_exit(int code)
{
	u_syscall1(SYS_EXIT, code);
	for (;;)
		;
}

#endif
