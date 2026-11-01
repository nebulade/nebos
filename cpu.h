/* CPU feature detection for the optional fast paths.
 *
 * The kernel targets i586, so SSE/SSE2 must never be assumed: CPUID itself
 * may not exist (early Pentium and below), which is why the ID bit in EFLAGS
 * is toggled first.  Detected features are cached after the first call. */
#ifndef CPU_H
#define CPU_H

/* 1 when the CPU supports SSE2 (the ION does, the Pentium targets do not). */
int cpu_has_sse2(void);

/* Enable CR4.OSFXSR/OSXMMEXCPT on the calling CPU when it has FXSR.  The BSP
 * does this lazily in cpu_has_sse2(); an AP starts with firmware CR4 and must
 * call this before it can run the SSE2 drawing fast paths. */
void cpu_enable_sse(void);

#endif
