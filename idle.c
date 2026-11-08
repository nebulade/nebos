/* CPU idle: automatic deep C-states on Nehalem/Westmere and Airmont/Braswell,
 * HLT elsewhere.
 *
 * The default is IDLE_AUTO, which asks power.c for the MWAIT hint of the
 * CPU: C6 (0x20) on the Arrandale/Nehalem family, C6N (0x58) on Braswell,
 * and -1 (HLT) on the older Atom, where the old USB-legacy SMM hang made
 * deep states risky.  The explicit c1e/c3/c6 modes remain for testing; the
 * c3ap/c6ap variants only apply to the APs.
 *
 * Arrandale MWAIT state hints (coreboot model_2065x): C1 0x00, C1E 0x01,
 * C3 0x10, C6 0x20.  Airmont uses the Cherry Trail C6N hint 0x58.  The
 * MWAIT ECX bit 0 (interrupts break the state) is set so a wake-up never
 * waits for the full hint duration.
 */
#include "idle.h"
#include "power.h"
#include "sched.h"

#define MWAIT_C1E 0x01
#define MWAIT_C3  0x10
#define MWAIT_C6  0x20

static int has_mwait;
static int has_mwait_ext;
static int current_mode = IDLE_AUTO;

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

	has_mwait = 0;
	has_mwait_ext = 0;
	current_mode = IDLE_AUTO;

	cpuid_idle(0, &max_leaf, &ebx, &ecx, &edx);
	if (max_leaf < 1)
		return;

	cpuid_idle(1, &eax, &ebx, &ecx, &edx);
	has_mwait = (ecx & (1u << 3)) ? 1 : 0;

	if (has_mwait && max_leaf >= 5) {
		cpuid_idle(5, &eax, &ebx, &ecx, &edx);
		has_mwait_ext = (ecx & 1u) ? 1 : 0;
	}
}

int idle_set_mode(int mode)
{
	if (mode == IDLE_AUTO) {
		current_mode = IDLE_AUTO;
		return 0;
	}
	if (mode == IDLE_C1) {
		current_mode = IDLE_C1;
		return 0;
	}
	if (!has_mwait || !has_mwait_ext)
		return -1;
	if (mode >= IDLE_C1E && mode <= IDLE_C6_AP) {
		current_mode = mode;
		return 0;
	}
	return -1;
}

int idle_mode(void)
{
	return current_mode;
}

int idle_mwait_supported(void)
{
	return has_mwait && has_mwait_ext;
}

const char *idle_mode_name(int mode)
{
	switch (mode) {
	case IDLE_C1E:
		return "mwait c1e";
	case IDLE_C3:
		return "mwait c3";
	case IDLE_C6:
		return "mwait c6";
	case IDLE_C3_AP:
		return "mwait c3 (APs)";
	case IDLE_C6_AP:
		return "mwait c6 (APs)";
	case IDLE_AUTO:
		return "auto";
	default:
		return "hlt";
	}
}

void cpu_idle(void)
{
	static unsigned int wake_addr;
	int mode = current_mode;
	int hint = -1;

	if (mode == IDLE_AUTO) {
		hint = power_idle_hint();
		if (hint >= 0 && (!has_mwait || !has_mwait_ext))
			hint = -1;
	} else if (mode != IDLE_C1) {
		int ap_only = (mode == IDLE_C3_AP || mode == IDLE_C6_AP);

		if (!has_mwait || !has_mwait_ext ||
		    (ap_only && this_cpu()->id == 0)) {
			__asm__ volatile ("hlt");
			return;
		}
		if (mode == IDLE_C1E)
			hint = MWAIT_C1E;
		else if (mode == IDLE_C3 || mode == IDLE_C3_AP)
			hint = MWAIT_C3;
		else
			hint = MWAIT_C6;
	}

	if (hint >= 0) {
		/* MONITOR/MWAIT on a cacheable address; ECX=1 in MWAIT lets
		 * an interrupt break the state immediately. */
		__asm__ volatile ("monitor"
				  : : "a"(&wake_addr), "c"(0), "d"(0));
		__asm__ volatile ("mwait" : : "a"((unsigned int)hint),
				  "c"(1));
		return;
	}
	__asm__ volatile ("hlt");
}
