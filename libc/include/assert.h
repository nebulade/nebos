/* Minimal freestanding assert.h for ring-3 commands. */
#ifndef LIBC_ASSERT_H
#define LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, unsigned int line)
	__attribute__((noreturn));

#define assert(expr) \
	((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
