/* CPU idle: HLT (C1) by default; optional MONITOR/MWAIT C2/C3.
 *
 * MWAIT used to be left off entirely on the ASUS AT3IONT-I (Atom 330 +
 * NVIDIA MCP79): a USB-legacy SMM event (the first key press) hung the
 * machine in C2.  The UHCI/OHCI drivers now disable that legacy SMI
 * emulation (PCI config 0xC0), so C2 is exposed through `idle c2` for a
 * runtime test.  HLT stays the default until the board has been tested;
 * `idle` with no argument shows the current mode.
 */
#include "idle.h"
#include "sched.h"

static int has_mwait;
static int has_mwait_ext;
static int current_mode = IDLE_C1;
static unsigned int mwait_hint = 0x10;

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
	current_mode = IDLE_C1;

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
	if (mode == IDLE_C1) {
		current_mode = IDLE_C1;
		return 0;
	}
	if (!has_mwait || !has_mwait_ext)
		return -1;
	if (mode == IDLE_C2) {
		mwait_hint = 0x10;
		current_mode = IDLE_C2;
		return 0;
	}
	if (mode == IDLE_C3) {
		mwait_hint = 0x20;
		current_mode = IDLE_C3;
		return 0;
	}
	if (mode == IDLE_C2_AP) {
		mwait_hint = 0x10;
		current_mode = IDLE_C2_AP;
		return 0;
	}
	if (mode == IDLE_C3_AP) {
		mwait_hint = 0x20;
		current_mode = IDLE_C3_AP;
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
	case IDLE_C2:
		return "mwait c2";
	case IDLE_C3:
		return "mwait c3";
	case IDLE_C2_AP:
		return "mwait c2 (APs)";
	case IDLE_C3_AP:
		return "mwait c3 (APs)";
	default:
		return "hlt";
	}
}

void cpu_idle(void)
{
	int ap_only = (current_mode == IDLE_C2_AP ||
		       current_mode == IDLE_C3_AP);

	if (current_mode != IDLE_C1 && has_mwait && has_mwait_ext &&
	    (!ap_only || this_cpu()->id != 0)) {
		volatile unsigned int dummy = 0;

		__asm__ volatile ("monitor" : : "a"(&dummy), "c"(0), "d"(0));
		__asm__ volatile ("mwait" : : "a"(mwait_hint), "c"(1));
		return;
	}
	__asm__ volatile ("hlt");
}
