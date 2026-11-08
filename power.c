/* Intel P-state and deep C-state management.
 *
 * Nehalem/Westmere (the X201) get the full race-to-idle P-state governor:
 * the module writes IA32_PERF_CTL (lowest ratio for idle CPUs, maximum
 * non-turbo ratio for a CPU that is running something, linear in between for
 * mixed windows) and idle.c uses C1E/C3/C6 MWAIT hints.
 *
 * Braswell/Airmont (the HP Chromebook, family 6 model 0x4C) reads the Atom
 * ratio/VID MSRs and MSR_FSB_FREQ, writes IA32_PERF_CTL with the pstate in
 * 15:8 and the Linux-interpolated VID in 7:0, and runs the same race-to-idle
 * governor.  It enters at the maximum non-turbo pstate (pstate 20 = 1600 MHz
 * on the N3060) and drops to pstate 6 (480 MHz) when idle; the turbo pstate
 * is detected but not requested.  The package/power-SKU and auto-demotion
 * MSRs are deliberately not touched (coreboot configures them, and an
 * unsupported rdmsr before the IDT exists triple-faults).  APERF/MPERF are
 * used only when CPUID.06H:ECX[0] says they exist.
 *
 * The module only touches documented MSRs and is gated on the model list;
 * unknown CPUs keep the old HLT behaviour.
 */
#include "power.h"
#include "bootcon.h"
#include "cpu.h"
#include "idle.h"
#include "io.h"
#include "sched.h"
#include "serial.h"

#define MSR_IA32_PERF_STATUS   0x198
#define MSR_IA32_PERF_CTL      0x199
#define MSR_PLATFORM_INFO      0x0CE
#define MSR_FSB_FREQ           0x0CD
#define MSR_MISC_ENABLE        0x1A0
#define MSR_MISC_PWR_MGMT      0x1AA
#define MSR_POWER_CTL          0x1FC
#define MSR_PKG_CST_CONFIG     0x0E2
#define MSR_PKG_C3_RESIDENCY   0x3F8
#define MSR_PKG_C6_RESIDENCY   0x3F9
#define MSR_CORE_C3_RESIDENCY  0x3FC
#define MSR_CORE_C6_RESIDENCY  0x3FD
#define MSR_APERF              0x0E8
#define MSR_MPERF              0x0E7
#define MSR_TSC                0x010
#define MSR_ATOM_CORE_RATIOS   0x66A
#define MSR_ATOM_CORE_VIDS     0x66B
#define MSR_ATOM_CORE_TURBO_RATIOS 0x66C
#define MSR_ATOM_CORE_TURBO_VIDS   0x66D

/* P-state ratios are the bus-ratio multipliers (133.33 MHz on Arrandale). */
#define BCLK_MHZ 133u

#define PLAT_NEHALEM 1
#define PLAT_AIRMONT 2

/* Airmont MWAIT C6N hint from Linux's intel_idle cht_cstates table. */
#define MWAIT_C6N 0x58

struct power_cpu {
	/* Governor state (100 ms windows). */
	unsigned int div;              /* ms in this window */
	unsigned int busy_ms;          /* ms this CPU ran a non-idle proc */
	unsigned int streak_ms;        /* consecutive busy ms */
	unsigned int req;              /* last ratio written */

	/* Once-a-second statistics. */
	unsigned int stat_div;
	int have_last;
	unsigned long long aperf, mperf, c3, c6, tsc;
	unsigned int mhz;
	unsigned int ratio;
	unsigned int status;           /* raw IA32_PERF_STATUS[15:0] */
	unsigned int pc3, pc6;         /* permille */
};

static struct power_cpu pcs[MAX_CPUS];

static int platform;
static int pstate_writes;
static int supported;
static int has_arat;
static int has_aperf;
static int eist_on;
static int turbo_disabled;
static int c1e_on;
static unsigned int cst_limit;
static int cst_locked;
static unsigned int min_ratio = 6;
static unsigned int max_ratio;
static unsigned int ratio_shift;
static unsigned int nominal_mhz;
static int cpufreq_mode = POWER_CPUFREQ_AUTO;
static unsigned int fixed_ratio;
static unsigned long long pkg_c3_last, pkg_c6_last;
static unsigned int pkg_pc3, pkg_pc6;
static int pkg_have_last;

/* Airmont probe values (kept for the `power` output and the follow-up that
 * enables the governor). */
static unsigned int fsb_khz;
static unsigned int turbo_ratio;
static unsigned int turbo_vid;
static unsigned int atom_vid_min;
static unsigned int atom_vid_max;
static int atom_vid_valid;
static unsigned int c6n_ok;
static int c6n_enabled;
static unsigned int plat_info_lo, plat_info_hi;

static void cpuid_leaf(unsigned int leaf, unsigned int *a, unsigned int *b,
		       unsigned int *c, unsigned int *d)
{
	__asm__ volatile ("cpuid"
			  : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			  : "a"(leaf), "c"(0));
}

static void rdmsr(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
	__asm__ volatile ("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static void wrmsr(unsigned int msr, unsigned int lo, unsigned int hi)
{
	__asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static unsigned long long rdmsr64(unsigned int msr)
{
	unsigned int lo, hi;

	rdmsr(msr, &lo, &hi);
	return ((unsigned long long)hi << 32) | lo;
}

/* Model allowlist.  Nehalem/Westmere implement the P-state/C-state MSRs
 * used here; Braswell is Airmont family 6 model 0x4C.  Bonnell
 * (0x1C/0x1D) is deliberately absent. */
static int power_platform(unsigned int model)
{
	switch (model) {
	case 0x1A:   /* Nehalem */
	case 0x1E:   /* Lynnfield/Clarksfield */
	case 0x1F:   /* Nehalem-EX */
	case 0x25:   /* Arrandale/Clarkdale (X201 i5 M 520) */
	case 0x2C:   /* Westmere-EP */
	case 0x2E:   /* Nehalem-EX */
	case 0x2F:   /* Westmere-EX */
		return PLAT_NEHALEM;
	case 0x4C:   /* Airmont: Braswell / Cherry Trail */
		return PLAT_AIRMONT;
	default:
		return 0;
	}
}

/* Airmont P-state VID for a target pstate.  Linux intel_pstate interpolates
 * linearly between the min/max VID from MSR_ATOM_CORE_VIDS; the P-unit uses
 * the low byte of IA32_PERF_CTL together with the ratio in 15:8. */
static unsigned int atom_vid_for(unsigned int ratio)
{
	unsigned int span = max_ratio - min_ratio;
	unsigned int v;

	if (!atom_vid_valid || !span)
		return 0;
	if (ratio <= min_ratio)
		return atom_vid_min & 0x7F;
	if (ratio >= max_ratio)
		return atom_vid_max & 0x7F;
	v = atom_vid_min +
	    ((ratio - min_ratio) * (atom_vid_max - atom_vid_min) +
	     span / 2) / span;
	return v & 0x7F;
}

/* Write one target ratio.  Nehalem/Westmere use bits 15:8 of
 * IA32_PERF_CTL for the target ratio; the low byte is reserved and written
 * as zero (coreboot does the same).  Airmont takes the ratio in 15:8 and the
 * interpolated VID in 7:0. */
static void power_write_ratio(unsigned int ratio)
{
	unsigned int lo, hi;

	if (ratio < min_ratio)
		ratio = min_ratio;
	if (ratio > max_ratio)
		ratio = max_ratio;
	rdmsr(MSR_IA32_PERF_CTL, &lo, &hi);
	if (platform == PLAT_AIRMONT) {
		lo = (lo & 0xFFFF0000u) | ((ratio & 0xFF) << 8) |
		     atom_vid_for(ratio);
	} else if (ratio_shift) {
		/* Bits 15:8 carry the ratio and 7:0 are reserved (the old VID
		 * slot); coreboot writes them as zero.  Keeping the BIOS VID
		 * byte here made the request silently ineffective. */
		lo = (lo & 0xFFFF0000u) | (ratio << 8);
	} else {
		lo = (lo & ~0xFFu) | ratio;
	}
	wrmsr(MSR_IA32_PERF_CTL, lo, hi);
}

static void power_apply(struct power_cpu *p, unsigned int ratio)
{
	if (ratio < min_ratio)
		ratio = min_ratio;
	if (ratio > max_ratio)
		ratio = max_ratio;
	if (ratio == p->req)
		return;
	power_write_ratio(ratio);
	p->req = ratio;
}

/* Airmont FSB frequencies in kHz, SDM Table 35-10 (index in MSR_FSB_FREQ). */
static unsigned int airmont_fsb(unsigned int index)
{
	static const unsigned int table[9] = {
		83300, 100000, 133300, 116700, 80000,
		93300, 90000, 88900, 87500
	};

	if (index >= sizeof(table) / sizeof(table[0]))
		return 100000;
	return table[index];
}

static void power_init_airmont(unsigned int max_leaf)
{
	unsigned int lo, hi, vlo, vhi, tlo, thi, slo, shi;
	unsigned int misc_lo, misc_hi;
	unsigned int idx, r, i;

	bootcon_puts("power: airmont init\n");
	rdmsr(MSR_PLATFORM_INFO, &plat_info_lo, &plat_info_hi);
	rdmsr(MSR_ATOM_CORE_RATIOS, &lo, &hi);
	min_ratio = (lo >> 8) & 0x7F;
	max_ratio = (lo >> 16) & 0x7F;

	/* If the Atom-specific MSR is not populated, fall back to the
	 * architectural PLATFORM_INFO fields. */
	if (max_ratio < 5 || max_ratio > 60 || min_ratio < 3 ||
	    min_ratio > max_ratio) {
		max_ratio = (plat_info_lo >> 8) & 0xFF;
		min_ratio = (plat_info_hi >> 8) & 0xFF;
	}
	if (max_ratio < 5 || max_ratio > 60 || min_ratio < 3 ||
	    min_ratio > max_ratio) {
		serial_puts(SERIAL_COM1,
			    "power: implausible Atom ratios, disabled\n");
		return;
	}
	bootcon_puts("power: ratios ok\n");

	rdmsr(MSR_FSB_FREQ, &lo, &hi);
	idx = lo & 0xF;
	fsb_khz = airmont_fsb(idx);
	nominal_mhz = (max_ratio * fsb_khz + 500) / 1000;
	if (!nominal_mhz) {
		serial_puts(SERIAL_COM1, "power: zero nominal frequency, disabled\n");
		return;
	}

	rdmsr(MSR_ATOM_CORE_VIDS, &vlo, &vhi);
	atom_vid_min = (vlo >> 8) & 0x7F;
	atom_vid_max = (vlo >> 16) & 0x7F;
	atom_vid_valid = (max_ratio > min_ratio &&
			  atom_vid_min <= atom_vid_max);

	rdmsr(MSR_ATOM_CORE_TURBO_RATIOS, &tlo, &thi);
	turbo_ratio = tlo & 0x7F;
	rdmsr(MSR_ATOM_CORE_TURBO_VIDS, &slo, &shi);
	turbo_vid = slo & 0x7F;

	rdmsr(MSR_MISC_ENABLE, &misc_lo, &misc_hi);
	eist_on = (misc_lo >> 16) & 1;
	turbo_disabled = (misc_hi >> 6) & 1;

	rdmsr(MSR_IA32_PERF_STATUS, &lo, &hi);
	r = (lo >> 8) & 0xFF;
	if (r >= min_ratio && r <= max_ratio) {
		pcs[0].ratio = r;
		pcs[0].req = r;
	} else {
		pcs[0].ratio = max_ratio;
		pcs[0].req = max_ratio;
	}
	pcs[0].status = lo & 0xFFFF;
	bootcon_puts("power: msrs ok\n");

	has_arat = 0;
	c6n_ok = 0;
	if (max_leaf >= 5) {
		unsigned int a, b, c, d;

		cpuid_leaf(5, &a, &b, &c, &d);
		c6n_ok = ((d >> 24) & 0xF) ? 1 : 0;
	}
	if (max_leaf >= 6) {
		unsigned int a, b, c, d;

		cpuid_leaf(6, &a, &b, &c, &d);
		has_arat = (a & 4u) ? 1 : 0;
		has_aperf = (c & 1u) ? 1 : 0;
	}

	c1e_on = 0;
	cst_limit = 0;
	cst_locked = 0;
	pkg_have_last = 0;
	/* The governor starts in auto: enter at max non-turbo, then drop to
	 * pstate 6 after the first idle window.  C6N is enabled when CPUID
	 * advertises the C6 substates. */
	cpufreq_mode = POWER_CPUFREQ_AUTO;
	supported = 1;
	pstate_writes = 1;
	for (i = 0; i < MAX_CPUS; i++) {
		pcs[i].req = 0;
		pcs[i].ratio = max_ratio;
	}
	c6n_enabled = c6n_ok ? 1 : 0;

	if (has_aperf) {
		pcs[0].aperf = rdmsr64(MSR_APERF);
		pcs[0].mperf = rdmsr64(MSR_MPERF);
	}
	pcs[0].have_last = 1;
	bootcon_puts("power: apis ok\n");

	/* Enter at the maximum non-turbo pstate; the race-to-idle governor
	 * drops to the minimum after the first idle window. */
	pcs[0].req = max_ratio;
	power_write_ratio(max_ratio);
	bootcon_puts("power: pstate writes enabled\n");
	if (c6n_enabled)
		bootcon_puts("power: c6n enabled\n");

	serial_puts(SERIAL_COM1,
		    "power: Airmont/Braswell, P-states auto");
	if (c6n_enabled)
		serial_puts(SERIAL_COM1, ", C6N enabled");
	else
		serial_puts(SERIAL_COM1, ", C6N disabled");
	serial_puts(SERIAL_COM1, "\n");
	serial_puts(SERIAL_COM1, "power: pstates ");
	serial_putdec(SERIAL_COM1, min_ratio);
	serial_puts(SERIAL_COM1, "..");
	serial_putdec(SERIAL_COM1, max_ratio);
	serial_puts(SERIAL_COM1, ", fsb ");
	serial_putdec(SERIAL_COM1, fsb_khz);
	serial_puts(SERIAL_COM1, " kHz, nominal ");
	serial_putdec(SERIAL_COM1, nominal_mhz);
	serial_puts(SERIAL_COM1, " MHz, vid ");
	serial_puthex(SERIAL_COM1, atom_vid_min);
	serial_puts(SERIAL_COM1, "..");
	serial_puthex(SERIAL_COM1, atom_vid_max);
	serial_puts(SERIAL_COM1, atom_vid_valid ? "" : " (invalid)");
	serial_puts(SERIAL_COM1, "\npower: turbo p");
	serial_putdec(SERIAL_COM1, turbo_ratio);
	serial_puts(SERIAL_COM1, " vid ");
	serial_puthex(SERIAL_COM1, turbo_vid);
	serial_puts(SERIAL_COM1, ", eist ");
	serial_puts(SERIAL_COM1, eist_on ? "on" : "off");
	serial_puts(SERIAL_COM1, ", turbo ");
	serial_puts(SERIAL_COM1, turbo_disabled ? "disabled" : "enabled");
	serial_puts(SERIAL_COM1, ", arat ");
	serial_puts(SERIAL_COM1, has_arat ? "yes" : "no");
	serial_puts(SERIAL_COM1, ", c6n ");
	serial_puts(SERIAL_COM1, c6n_ok ? "yes" : "no");
	serial_puts(SERIAL_COM1, "\npower: platform info ");
	serial_puthex(SERIAL_COM1, plat_info_hi);
	serial_puthex(SERIAL_COM1, plat_info_lo);
	serial_puts(SERIAL_COM1, ", perf status ");
	serial_puthex(SERIAL_COM1, pcs[0].status);
	serial_puts(SERIAL_COM1, ", perf ctl ");
	rdmsr(MSR_IA32_PERF_CTL, &lo, &hi);
	serial_puthex(SERIAL_COM1, lo);
	serial_puts(SERIAL_COM1, "\npower: misc enable ");
	serial_puthex(SERIAL_COM1, misc_lo);
	serial_puts(SERIAL_COM1, "\n");
}

void power_init(void)
{
	unsigned int max_leaf, a, b, c, d;
	unsigned int family, model;
	unsigned int lo, hi;

	supported = 0;
	pstate_writes = 0;
	platform = 0;
	has_arat = 0;
	has_aperf = 0;
	eist_on = 0;
	turbo_disabled = 0;
	c1e_on = 0;
	cst_limit = 0;
	cst_locked = 0;
	min_ratio = 6;
	max_ratio = 0;
	ratio_shift = 0;
	nominal_mhz = 0;
	cpufreq_mode = POWER_CPUFREQ_AUTO;
	fixed_ratio = 0;
	fsb_khz = 0;
	turbo_ratio = 0;
	turbo_vid = 0;
	atom_vid_min = 0;
	atom_vid_max = 0;
	atom_vid_valid = 0;
	c6n_ok = 0;
	c6n_enabled = 0;
	plat_info_lo = 0;
	plat_info_hi = 0;
	pkg_have_last = 0;
	pkg_pc3 = 0;
	pkg_pc6 = 0;

	if (!cpu_has_cpuid()) {
		serial_puts(SERIAL_COM1, "power: no CPUID, disabled\n");
		return;
	}

	cpuid_leaf(0, &max_leaf, &b, &c, &d);
	if (max_leaf < 1) {
		serial_puts(SERIAL_COM1, "power: no CPUID leaf 1\n");
		return;
	}
	cpuid_leaf(1, &a, &b, &c, &d);
	family = (a >> 8) & 0xF;
	model = (a >> 4) & 0xF;
	if (family == 0xF)
		family += (a >> 20) & 0xFF;
	if (family == 0x6 || family == 0xF)
		model += ((a >> 16) & 0xF) << 4;
	platform = (family == 0x6) ? power_platform(model) : 0;
	if (!platform) {
		serial_puts(SERIAL_COM1, "power: CPU family ");
		serial_putdec(SERIAL_COM1, family);
		serial_puts(SERIAL_COM1, " model ");
		serial_putdec(SERIAL_COM1, model);
		serial_puts(SERIAL_COM1, ", no supported PM\n");
		return;
	}

	if (platform == PLAT_AIRMONT) {
		power_init_airmont(max_leaf);
		return;
	}

	rdmsr(MSR_PLATFORM_INFO, &lo, &hi);
	max_ratio = (lo >> 8) & 0xFF;
	min_ratio = (hi >> 8) & 0xFF;
	if (max_ratio < 5 || max_ratio > 60 || min_ratio < 3 ||
	    min_ratio > max_ratio) {
		serial_puts(SERIAL_COM1, "power: implausible platform info, disabled\n");
		return;
	}
	nominal_mhz = max_ratio * BCLK_MHZ + max_ratio / 3;

	/* Nehalem/Westmere keep the target ratio in bits 15:8 of
	 * IA32_PERF_CTL/STATUS; the low byte is the (now unused) VID field.
	 * coreboot's set_max_ratio writes (ratio << 8) for this family, and
	 * reading the low byte on the X201 gives VID-looking values (0x11,
	 * 0x18) that would otherwise masquerade as ratios. */
	rdmsr(MSR_IA32_PERF_STATUS, &lo, &hi);
	ratio_shift = 8;

	if (max_leaf >= 6) {
		cpuid_leaf(6, &a, &b, &c, &d);
		has_arat = (a & 4u) ? 1 : 0;
	}

	rdmsr(MSR_MISC_ENABLE, &lo, &hi);
	eist_on = (lo >> 16) & 1;
	turbo_disabled = (hi >> 6) & 1;
	if (!eist_on) {
		wrmsr(MSR_MISC_ENABLE, lo | (1u << 16), hi);
		eist_on = 1;
	}

	rdmsr(MSR_POWER_CTL, &lo, &hi);
	c1e_on = (lo >> 1) & 1;
	if (!c1e_on) {
		wrmsr(MSR_POWER_CTL, lo | (1u << 1), hi);
		c1e_on = 1;
	}

	rdmsr(MSR_PKG_CST_CONFIG, &lo, &hi);
	cst_limit = lo & 7u;
	cst_locked = (lo >> 15) & 1;
	if (!cst_locked && cst_limit < 3) {
		wrmsr(MSR_PKG_CST_CONFIG, (lo & ~7u) | 3u, hi);
		cst_limit = 3;
	}

	rdmsr(MSR_MISC_PWR_MGMT, &lo, &hi);
	supported = 1;
	pstate_writes = 1;
	has_aperf = 1;

	/* Start at the maximum non-turbo ratio; the governor drops to the
	 * minimum after the first idle window. */
	rdmsr(MSR_IA32_PERF_STATUS, &lo, &hi);
	serial_puts(SERIAL_COM1, "power: perf status ");
	serial_puthex(SERIAL_COM1, lo);
	pcs[0].req = max_ratio;
	power_write_ratio(max_ratio);
	rdmsr(MSR_IA32_PERF_CTL, &lo, &hi);
	serial_puts(SERIAL_COM1, ", perf ctl ");
	serial_puthex(SERIAL_COM1, lo);
	rdmsr(MSR_IA32_PERF_STATUS, &lo, &hi);
	serial_puts(SERIAL_COM1, ", status now ");
	serial_puthex(SERIAL_COM1, lo);
	serial_puts(SERIAL_COM1, "\n");
	pcs[0].aperf = rdmsr64(MSR_APERF);
	pcs[0].mperf = rdmsr64(MSR_MPERF);
	pcs[0].c3 = rdmsr64(MSR_CORE_C3_RESIDENCY);
	pcs[0].c6 = rdmsr64(MSR_CORE_C6_RESIDENCY);
	pcs[0].tsc = rdmsr64(MSR_TSC);
	pcs[0].have_last = 1;
	pkg_c3_last = rdmsr64(MSR_PKG_C3_RESIDENCY);
	pkg_c6_last = rdmsr64(MSR_PKG_C6_RESIDENCY);
	pkg_have_last = 1;

	serial_puts(SERIAL_COM1, "power: Nehalem/Westmere, ratios ");
	serial_putdec(SERIAL_COM1, min_ratio);
	serial_puts(SERIAL_COM1, "..");
	serial_putdec(SERIAL_COM1, max_ratio);
	serial_puts(SERIAL_COM1, " (");
	serial_putdec(SERIAL_COM1, nominal_mhz);
	serial_puts(SERIAL_COM1, " MHz), perfctl shift ");
	serial_putdec(SERIAL_COM1, ratio_shift);
	serial_puts(SERIAL_COM1, "\npower: eist ");
	serial_puts(SERIAL_COM1, eist_on ? "on" : "off");
	serial_puts(SERIAL_COM1, ", turbo ");
	serial_puts(SERIAL_COM1, turbo_disabled ? "disabled" : "enabled");
	serial_puts(SERIAL_COM1, ", c1e ");
	serial_puts(SERIAL_COM1, c1e_on ? "on" : "off");
	serial_puts(SERIAL_COM1, ", cst ");
	serial_putdec(SERIAL_COM1, cst_limit);
	serial_puts(SERIAL_COM1, cst_locked ? " locked" : " unlocked");
	serial_puts(SERIAL_COM1, ", arat ");
	serial_puts(SERIAL_COM1, has_arat ? "yes" : "no");
	serial_puts(SERIAL_COM1, ", pwr mgmt ");
	serial_puthex(SERIAL_COM1, lo);
	serial_puts(SERIAL_COM1, "\n");
}

int power_idle_hint(void)
{
	if (!supported)
		return -1;
	if (platform == PLAT_AIRMONT) {
		/* C6N is reported by CPUID, but the hint stays disabled
		 * until the read-only probe has booted cleanly on target. */
		if (!c6n_enabled || !c6n_ok)
			return -1;
		/* An AP without ARAT may lose its local APIC timer, so it
		 * falls back to HLT (C1). */
		if (this_cpu()->id != 0 && !has_arat)
			return -1;
		return MWAIT_C6N;
	}
	/* An AP can only use C3/C6 when its local APIC timer keeps running
	 * there (ARAT); otherwise C1E is the deepest safe state and the BSP
	 * PIT still wakes it up via IPIs. */
	if (this_cpu()->id != 0 && !has_arat)
		return 0x01;
	return 0x20;   /* C6 hint: the CPU uses the deepest state it has */
}

void power_tick(int idle, unsigned int ms)
{
	struct cpu *c = this_cpu();
	struct power_cpu *p;
	unsigned int load, r;

	if (!supported || c->id >= MAX_CPUS || ms == 0)
		return;
	p = &pcs[c->id];

	/* ---- P-state governor (100 ms windows) ---- */
	if (pstate_writes && cpufreq_mode == POWER_CPUFREQ_AUTO) {
		if (idle) {
			p->streak_ms = 0;
		} else {
			p->busy_ms += ms;
			if (p->streak_ms < 1000)
				p->streak_ms += ms;
			/* A short busy streak means interactive work or a
			 * command: jump to max immediately, then let the next
			 * window decide. */
			if (p->streak_ms >= 4 && p->req != max_ratio)
				power_apply(p, max_ratio);
		}
		p->div += ms;
		if (p->div >= 100) {
			load = p->busy_ms * 100u / p->div;
			p->div = 0;
			p->busy_ms = 0;
			if (load < 15)
				r = min_ratio;
			else if (load > 80)
				r = max_ratio;
			else
				r = min_ratio +
				    (max_ratio - min_ratio) * load / 100u;
			power_apply(p, r);
		}
	} else if (pstate_writes && cpufreq_mode != POWER_CPUFREQ_OFF) {
		if (cpufreq_mode == POWER_CPUFREQ_MIN)
			r = min_ratio;
		else if (cpufreq_mode == POWER_CPUFREQ_MAX)
			r = max_ratio;
		else
			r = fixed_ratio;
		power_apply(p, r);
	}

	/* ---- once-a-second clock statistics ---- */
	p->stat_div += ms;
	if (p->stat_div < 1000)
		return;
	p->stat_div = 0;
	if (platform == PLAT_AIRMONT) {
		unsigned long long aperf, mperf;
		unsigned int da, dm;

		if (!has_aperf) {
			/* The CLOCK_MODULATION/APERF capability is absent;
			 * report the PERF_STATUS ratio only. */
			rdmsr(MSR_IA32_PERF_STATUS, &da, &dm);
			p->status = da & 0xFFFFu;
			r = (p->status >> 8) & 0xFFu;
			if (r >= min_ratio && r <= max_ratio)
				p->ratio = r;
			else
				p->ratio = p->req;
			return;
		}
		aperf = rdmsr64(MSR_APERF);
		mperf = rdmsr64(MSR_MPERF);
		if (!p->have_last) {
			p->aperf = aperf;
			p->mperf = mperf;
			p->have_last = 1;
			return;
		}
		da = (unsigned int)((aperf - p->aperf) >> 12);
		dm = (unsigned int)((mperf - p->mperf) >> 12);
		p->aperf = aperf;
		p->mperf = mperf;
		p->mhz = dm ? da * nominal_mhz / dm : 0;
		rdmsr(MSR_IA32_PERF_STATUS, &da, &dm);
		p->status = da & 0xFFFFu;
		r = (p->status >> 8) & 0xFFu;
		if (r >= min_ratio && r <= max_ratio)
			p->ratio = r;
		else
			p->ratio = p->req;
		return;
	}
	{
		unsigned long long aperf = rdmsr64(MSR_APERF);
		unsigned long long mperf = rdmsr64(MSR_MPERF);
		unsigned long long tsc = rdmsr64(MSR_TSC);
		unsigned long long c3 = rdmsr64(MSR_CORE_C3_RESIDENCY);
		unsigned long long c6 = rdmsr64(MSR_CORE_C6_RESIDENCY);
		unsigned int dtsc, d3, d6, da, dm;

		if (!p->have_last) {
			p->aperf = aperf;
			p->mperf = mperf;
			p->c3 = c3;
			p->c6 = c6;
			p->tsc = tsc;
			p->have_last = 1;
			return;
		}
		dtsc = (unsigned int)((tsc - p->tsc) >> 12);
		d3 = (unsigned int)((c3 - p->c3) >> 12);
		d6 = (unsigned int)((c6 - p->c6) >> 12);
		da = (unsigned int)((aperf - p->aperf) >> 12);
		dm = (unsigned int)((mperf - p->mperf) >> 12);
		p->aperf = aperf;
		p->mperf = mperf;
		p->c3 = c3;
		p->c6 = c6;
		p->tsc = tsc;

		p->pc3 = dtsc ? d3 * 1000u / dtsc : 0;
		p->pc6 = dtsc ? d6 * 1000u / dtsc : 0;
		if (p->pc3 > 1000)
			p->pc3 = 1000;
		if (p->pc6 > 1000)
			p->pc6 = 1000;
		p->mhz = dm ? da * nominal_mhz / dm : 0;
		/* IA32_PERF_STATUS reports the current ratio in bits 7:0
		 * (the boot dump shows 0x12 = 18 at the max non-turbo ratio),
		 * unlike PERF_CTL, which takes the target in bits 15:8. */
		rdmsr(MSR_IA32_PERF_STATUS, &da, &dm);
		p->status = da & 0xFFFFu;
		p->ratio = p->status & 0xFFu;

		if (c->id == 0) {
			unsigned long long pc3 = rdmsr64(MSR_PKG_C3_RESIDENCY);
			unsigned long long pc6 = rdmsr64(MSR_PKG_C6_RESIDENCY);

			if (pkg_have_last) {
				pkg_pc3 = dtsc ?
					(unsigned int)((pc3 - pkg_c3_last) >> 12) *
					1000u / dtsc : 0;
				pkg_pc6 = dtsc ?
					(unsigned int)((pc6 - pkg_c6_last) >> 12) *
					1000u / dtsc : 0;
				if (pkg_pc3 > 1000)
					pkg_pc3 = 1000;
				if (pkg_pc6 > 1000)
					pkg_pc6 = 1000;
			}
			pkg_c3_last = pc3;
			pkg_c6_last = pc6;
			pkg_have_last = 1;
		}
	}
}

const char *power_cpufreq_name(void)
{
	switch (cpufreq_mode) {
	case POWER_CPUFREQ_MIN:
		return "min";
	case POWER_CPUFREQ_MAX:
		return "max";
	case POWER_CPUFREQ_OFF:
		return "off";
	case POWER_CPUFREQ_FIXED:
		return "fixed";
	default:
		return "auto";
	}
}

static void put_dec(unsigned int v)
{
	char buf[12];
	unsigned int n = 0;

	if (!v) {
		io_putchar('0');
		return;
	}
	while (v && n < sizeof(buf)) {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (n)
		io_putchar(buf[--n]);
}

static void put_pct(unsigned int permille)
{
	put_dec(permille / 10);
	io_putchar('.');
	io_putchar((char)('0' + permille % 10));
	io_putchar('%');
}

void power_print(void)
{
	unsigned int i;
	unsigned int lo, hi;

	if (!supported) {
		io_putstr("power: not available on this CPU\n");
		return;
	}
	io_putstr("power: ");
	io_putstr(platform == PLAT_AIRMONT ? "airmont/braswell" :
					    "nehalem/westmere");
	if (!pstate_writes)
		io_putstr(" (probe only)");
	io_putstr(", idle ");
	io_putstr(idle_mode_name(idle_mode()));
	if (platform == PLAT_AIRMONT)
		io_putstr((c6n_enabled && idle_mwait_supported()) ?
			  " (mwait c6n)" : " (hlt)");
	io_putstr(", cpufreq ");
	if (!pstate_writes)
		io_putstr("probe");
	else {
		io_putstr(power_cpufreq_name());
		if (cpufreq_mode == POWER_CPUFREQ_FIXED) {
			io_putchar(' ');
			put_dec(fixed_ratio);
		}
	}
	io_putstr(" (pstates ");
	put_dec(min_ratio);
	io_putstr("..");
	put_dec(max_ratio);
	io_putstr(")\n");

	if (platform == PLAT_AIRMONT) {
		io_putstr("eist ");
		io_putstr(eist_on ? "on" : "off");
		io_putstr(", turbo ");
		io_putstr(turbo_disabled ? "disabled" : "enabled");
		io_putstr(", arat ");
		io_putstr(has_arat ? "yes" : "no");
		io_putstr(", c6n ");
		if (!c6n_ok)
			io_putstr("no");
		else if (c6n_enabled)
			io_putstr("yes");
		else
			io_putstr("yes (disabled)");
		io_putstr("\nfsb ");
		put_dec(fsb_khz);
		io_putstr(" kHz, nominal ");
		put_dec(nominal_mhz);
		io_putstr(" MHz, turbo p");
		put_dec(turbo_ratio);
		io_putstr(" vid 0x");
		io_puthex(turbo_vid);
		io_putstr(", vid 0x");
		io_puthex(atom_vid_min);
		io_putstr("..0x");
		io_puthex(atom_vid_max);
		io_putstr(atom_vid_valid ? "" : " (invalid)");
		rdmsr(MSR_IA32_PERF_CTL, &lo, &hi);
		io_putstr(", perf ctl 0x");
		io_puthex(lo);
		io_putchar('\n');
		for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
			struct power_cpu *p = &pcs[i];

			io_putstr("cpu");
			put_dec(i);
			io_putchar(' ');
			put_dec(p->mhz);
			io_putstr(" MHz  cur r");
			if (p->ratio >= min_ratio && p->ratio <= max_ratio) {
				if (p->ratio < 10)
					io_putchar('0');
				put_dec(p->ratio);
			} else {
				io_putstr("--");
			}
			io_putstr("  req r");
			if (p->req < 10)
				io_putchar('0');
			put_dec(p->req);
			io_putchar('\n');
		}
		return;
	}

	io_putstr("cst ");
	put_dec(cst_limit);
	io_putstr(cst_locked ? " locked" : " unlocked");
	io_putstr(", c1e ");
	io_putstr(c1e_on ? "on" : "off");
	io_putstr(", arat ");
	io_putstr(has_arat ? "yes\n" : "no\n");
	io_putstr("pkg c3 ");
	put_pct(pkg_pc3);
	io_putstr("  c6 ");
	put_pct(pkg_pc6);
	io_putchar('\n');
	for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
		struct power_cpu *p = &pcs[i];

		io_putstr("cpu");
		put_dec(i);
		io_putchar(' ');
		put_dec(p->mhz);
		io_putstr(" MHz  cur r");
		if (p->ratio >= min_ratio && p->ratio <= max_ratio + 8) {
			if (p->ratio < 10)
				io_putchar('0');
			put_dec(p->ratio);
		} else {
			io_putstr("--");
		}
		io_putstr("  req r");
		if (p->req < 10)
			io_putchar('0');
		put_dec(p->req);
		io_putstr("  c3 ");
		put_pct(p->pc3);
		io_putstr("  c6 ");
		put_pct(p->pc6);
		io_putchar('\n');
	}
}

int power_cpufreq_command(const char *arg)
{
	unsigned int ratio = 0;
	int have_ratio = 0;

	while (*arg == ' ')
		arg++;
	if (!supported) {
		io_putstr("cpufreq: not available on this CPU\n");
		return -1;
	}
	if (!pstate_writes) {
		if (!*arg) {
			io_putstr("cpufreq probe (P-state writes disabled)\n");
			return 0;
		}
		io_putstr("cpufreq: P-state writes are disabled in this probe build\n");
		return -1;
	}
	if (!*arg) {
		io_putstr("cpufreq ");
		io_putstr(power_cpufreq_name());
		if (cpufreq_mode == POWER_CPUFREQ_FIXED) {
			io_putchar(' ');
			put_dec(fixed_ratio);
		}
		io_putchar('\n');
		return 0;
	}
	if (arg[0] == 'a' && arg[1] == 'u' && arg[2] == 't' &&
	    arg[3] == 'o' && !arg[4]) {
		cpufreq_mode = POWER_CPUFREQ_AUTO;
	} else if (arg[0] == 'm' && arg[1] == 'i' && arg[2] == 'n' &&
		   !arg[3]) {
		cpufreq_mode = POWER_CPUFREQ_MIN;
	} else if (arg[0] == 'm' && arg[1] == 'a' && arg[2] == 'x' &&
		   !arg[3]) {
		cpufreq_mode = POWER_CPUFREQ_MAX;
	} else if (arg[0] == 'o' && arg[1] == 'f' && arg[2] == 'f' &&
		   !arg[3]) {
		cpufreq_mode = POWER_CPUFREQ_OFF;
	} else {
		while (*arg >= '0' && *arg <= '9') {
			ratio = ratio * 10u + (unsigned int)(*arg - '0');
			arg++;
			have_ratio = 1;
		}
		if (!have_ratio || *arg || ratio < min_ratio ||
		    ratio > max_ratio) {
			io_putstr("cpufreq: usage cpufreq [auto|min|max|off|<ratio>]\n");
			return -1;
		}
		fixed_ratio = ratio;
		cpufreq_mode = POWER_CPUFREQ_FIXED;
	}
	io_putstr("cpufreq ");
	io_putstr(power_cpufreq_name());
	if (cpufreq_mode == POWER_CPUFREQ_FIXED) {
		io_putchar(' ');
		put_dec(fixed_ratio);
	}
	io_putchar('\n');
	return 0;
}
