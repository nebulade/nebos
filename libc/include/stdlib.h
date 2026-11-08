/* Minimal freestanding stdlib.h for ring-3 commands. */
#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int abs(int value);
int atoi(const char *str);
long atol(const char *str);
long strtol(const char *str, char **endptr, int base);
void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *));

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

#endif
