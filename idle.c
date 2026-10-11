/* CPU idle: HLT (C1) by default; optional MONITOR/MWAIT (C2).
 *
 * NOTE: MWAIT is intentionally disabled.  On the ASUS AT3IONT-I (Atom 330 +
 * NVIDIA MCP79) a USB-legacy SMM event (the first key press) hangs the
 * machine when the CPU idles via MWAIT C2.  HLT is the path QEMU always used
 * (its default CPU has no MONITOR/MWAIT) and what every BIOS's legacy-USB
 * emulation expects, since DOS idles with HLT.
 */
#include "idle.h"

#define IDLE_USE_MWAIT 0

#if IDLE_USE_MWAIT
static int has_mwait = 0;
static int has_mwait_ext = 0;
static unsigned int mwait_cstate_hint = 0x00;

static void cpuid_idle(unsigned int leaf, unsigned int *eax, unsigned int *ebx,
		       unsigned int *ecx, unsigned int *edx)
{
	__asm__ volatile ("cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf), "c"(0));
}
#endif

void idle_init(void)
{
#if IDLE_USE_MWAIT
	unsigned int eax, ebx, ecx, edx;
	unsigned int max_leaf;

	cpuid_idle(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < 1)
		return;

	cpuid_idle(1, &eax, &ebx, &ecx, &edx);
	has_mwait = (ecx & (1u << 3)) ? 1 : 0;

	if (has_mwait && max_leaf >= 5) {
		cpuid_idle(5, &eax, &ebx, &ecx, &edx);
		has_mwait_ext = (ecx & 1u) ? 1 : 0;
	}

	mwait_cstate_hint = 0x10;
#endif
}

void cpu_idle(void)
{
#if IDLE_USE_MWAIT
	if (has_mwait && has_mwait_ext) {
		volatile unsigned int dummy = 0;
		__asm__ volatile (
			"monitor"
			:
			: "a"(&dummy), "c"(0), "d"(0)
		);
		__asm__ volatile (
			"mwait"
			:
			: "a"(mwait_cstate_hint), "c"(1)
		);
		return;
	}
#endif
	__asm__ volatile ("hlt");
}
