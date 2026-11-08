/* Minimal i386 setjmp/longjmp for the ring-3 libc. */
#ifndef LIBC_SETJMP_H
#define LIBC_SETJMP_H

/* ebx, esi, edi, ebp, esp, eip */
typedef unsigned int jmp_buf[6];

int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

#endif
