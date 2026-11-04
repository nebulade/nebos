/* Intel Nehalem/Westmere power management: P-states and deep C-states.
 *
 * The kernel has no ACPI interpreter and no cpufreq driver, so the CPU used
 * to stay at whatever P-state the BIOS left (the maximum non-turbo ratio)
 * and only ever reached C1 (HLT).  An idle X201 therefore ran at 2.4 GHz
 * with all four threads in C1, which kept the package near 65 C and the EC
 * fan around 3700 RPM.
 *
 * This module adds the two things the hardware needs from the OS:
 *  - a small race-to-idle P-state governor writing IA32_PERF_CTL (lowest
 *    ratio for idle CPUs, maximum non-turbo ratio for a CPU that is running
 *    something, linear in between for mixed windows), and
 *  - deep C-state hints for idle.c (C1E/C3/C6 through MONITOR/MWAIT).
 *
 * Everything is gated on the Nehalem/Westmere model list: those are the
 * parts known to implement the MSRs used here, and the Atom boards keep
 * their old HLT behaviour.  The module only touches documented MSRs.
 */
#include "power.h"
#include "cpu.h"
#include "idle.h"
#include "io.h"
#include "sched.h"
#include "serial.h"

#define MSR_IA32_PERF_STATUS   0x198
#define MSR_IA32_PERF_CTL      0x199
#define MSR_PLATFORM_INFO      0x0CE
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

/* P-state ratios are the bus-ratio multipliers (133.33 MHz on Arrandale). */
#define BCLK_MHZ 133u

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

static int supported;
static int has_arat;
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

/* Model allowlist: the Nehalem/Westmere family that implements the MSRs
 * above.  Bonnell (0x1C/0x1D) is deliberately absent. */
static int power_model(unsigned int model)
{
	switch (model) {
	case 0x1A:   /* Nehalem */
	case 0x1E:   /* Lynnfield/Clarksfield */
	case 0x1F:   /* Nehalem-EX */
	case 0x25:   /* Arrandale/Clarkdale (X201 i5 M 520) */
	case 0x2C:   /* Westmere-EP */
	case 0x2E:   /* Nehalem-EX */
	case 0x2F:   /* Westmere-EX */
		return 1;
	default:
		return 0;
	}
}

/* Write one target ratio.  Nehalem/Westmere use bits 15:8 of
 * IA32_PERF_CTL for the target ratio; the low byte is reserved and written
 * as zero (coreboot does the same). */
static void power_write_ratio(unsigned int ratio)
{
	unsigned int lo, hi;

	if (ratio < min_ratio)
		ratio = min_ratio;
	if (ratio > max_ratio)
		ratio = max_ratio;
	rdmsr(MSR_IA32_PERF_CTL, &lo, &hi);
	if (ratio_shift) {
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

void power_init(void)
{
	unsigned int max_leaf, a, b, c, d;
	unsigned int family, model;
	unsigned int lo, hi;

	supported = 0;
	has_arat = 0;
	cpufreq_mode = POWER_CPUFREQ_AUTO;
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
	if (family != 0x6 || !power_model(model)) {
		serial_puts(SERIAL_COM1, "power: CPU family ");
		serial_putdec(SERIAL_COM1, family);
		serial_puts(SERIAL_COM1, " model ");
		serial_putdec(SERIAL_COM1, model);
		serial_puts(SERIAL_COM1, ", no Nehalem/Westmere PM\n");
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
	if (cpufreq_mode == POWER_CPUFREQ_AUTO) {
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
	} else if (cpufreq_mode != POWER_CPUFREQ_OFF) {
		if (cpufreq_mode == POWER_CPUFREQ_MIN)
			r = min_ratio;
		else if (cpufreq_mode == POWER_CPUFREQ_MAX)
			r = max_ratio;
		else
			r = fixed_ratio;
		power_apply(p, r);
	}

	/* ---- once-a-second residency/clock statistics ---- */
	p->stat_div += ms;
	if (p->stat_div < 1000)
		return;
	p->stat_div = 0;
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

	if (!supported) {
		io_putstr("power: not available on this CPU\n");
		return;
	}
	io_putstr("power: nehalem/westmere, idle ");
	io_putstr(idle_mode_name(idle_mode()));
	io_putstr(", cpufreq ");
	io_putstr(power_cpufreq_name());
	if (cpufreq_mode == POWER_CPUFREQ_FIXED) {
		io_putchar(' ');
		put_dec(fixed_ratio);
	}
	io_putstr(" (ratios ");
	put_dec(min_ratio);
	io_putstr("..");
	put_dec(max_ratio);
	io_putstr(")\n");
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
