/* CPU feature detection (see cpu.h). */

#include "cpu.h"

static int sse2 = -1;      /* -1 = not probed yet, 0 = no, 1 = yes */

/* CPUID exists only if the ID bit in EFLAGS can be toggled (i586+). */
static int cpuid_supported(void)
{
	unsigned int a, b;

	__asm__ volatile ("pushfl; popl %0" : "=r"(a));
	b = a ^ (1u << 21);
	__asm__ volatile ("pushl %0; popfl" :: "r"(b));
	__asm__ volatile ("pushfl; popl %0" : "=r"(b));
	return ((a ^ b) & (1u << 21)) != 0;
}

void cpu_enable_sse(void)
{
	unsigned int eax, ebx, ecx, edx, cr4;

	if (!cpuid_supported())
		return;
	eax = 1;
	__asm__ volatile ("cpuid"
			  : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	if (!(edx & (1u << 24)))        /* FXSR */
		return;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1u << 9) | (1u << 10);  /* OSFXSR | OSXMMEXCPT */
	__asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

int cpu_has_sse2(void)
{
	unsigned int eax, ebx, ecx, edx;

	if (sse2 >= 0)
		return sse2;
	sse2 = 0;
	if (cpuid_supported()) {
		eax = 1;
		__asm__ volatile ("cpuid"
				  : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
		if (edx & (1u << 26)) {
			/* SSE instructions #UD unless CR4 enables the state
			 * (OSFXSR).  The kernel is built -mno-sse, so the
			 * registers are only used inside a single helper and
			 * need no per-process save. */
			if (edx & (1u << 24)) {          /* FXSR */
				unsigned int cr4;

				__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
				cr4 |= (1u << 9) | (1u << 10);  /* OSFXSR | OSXMMEXCPT */
				__asm__ volatile ("mov %0, %%cr4" :: "r"(cr4)
						  : "memory");
			}
			sse2 = 1;
		}
	}
	return sse2;
}
