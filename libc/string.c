/* Minimal freestanding string/memory functions for ring-3 commands. */

#include <stddef.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else if (d > s) {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;

	while (n--) {
		if (*pa != *pb)
			return (int)*pa - (int)*pb;
		pa++;
		pb++;
	}
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++) != '\0')
		;
	return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
	char *d = dst;

	while (n && *src) {
		*d++ = *src++;
		n--;
	}
	while (n--)
		*d++ = '\0';
	return dst;
}

char *strcat(char *dst, const char *src)
{
	char *d = dst;

	while (*d)
		d++;
	while ((*d++ = *src++) != '\0')
		;
	return dst;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (n == 0)
		return 0;
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
	char ch = (char)c;

	for (;; s++) {
		if (*s == ch)
			return (char *)s;
		if (*s == '\0')
			return 0;
	}
}

char *strrchr(const char *s, int c)
{
	char ch = (char)c;
	const char *last = 0;

	for (;; s++) {
		if (*s == ch)
			last = s;
		if (*s == '\0')
			return (char *)last;
	}
}

char *strstr(const char *haystack, const char *needle)
{
	const char *h;
	const char *n;

	if (!*needle)
		return (char *)haystack;

	for (; *haystack; haystack++) {
		h = haystack;
		n = needle;
		while (*h && *n && *h == *n) {
			h++;
			n++;
		}
		if (!*n)
			return (char *)haystack;
	}
	return 0;
}
