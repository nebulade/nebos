/* CPU idle: use mwait for deep C-states when available, hlt fallback. */

#include "idle.h"

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

void idle_init(void)
{
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
}

void idle_set_cstate(unsigned int level)
{
	switch (level) {
	case 0:
		mwait_cstate_hint = 0x00;
		break;
	case 1:
		mwait_cstate_hint = 0x10;
		break;
	case 2:
		mwait_cstate_hint = 0x20;
		break;
	case 3:
		mwait_cstate_hint = 0x30;
		break;
	default:
		mwait_cstate_hint = 0x10;
		break;
	}
}

unsigned int idle_get_cstate(void)
{
	return (mwait_cstate_hint >> 4) & 0xF;
}

int idle_has_mwait(void)
{
	return has_mwait;
}

void cpu_idle(void)
{
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
	} else {
		__asm__ volatile ("hlt");
	}
}
