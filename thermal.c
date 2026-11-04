/* Intel digital thermal sensor (see thermal.h).
 *
 * IA32_THERM_STATUS (MSR 0x19C) reports the core temperature as a delta
 * below TjMax in bits 22:16.  Bonnell (Atom 230/330) has no
 * IA32_TEMPERATURE_TARGET MSR, so TjMax has to come from a table; these are
 * the same values Linux's coretemp uses (the 330's DTS is calibrated against
 * 125 C, other Bonnell parts against 90 C).  Nehalem and later (including the
 * X201's Arrandale) publish TjMax in bits 23:16 of MSR 0x1A2, which is read
 * for the models that implement it; the table remains the fallback.  The DTS
 * "valid" bit is ignored on purpose: coretemp does the same because the delta
 * stays meaningful (the bit can be clear at temperatures below the sensor's
 * range).
 */
#include "thermal.h"
#include "cpu.h"
#include "sched.h"
#include "serial.h"
#include "timer.h"

#define MSR_IA32_THERM_STATUS        0x19C
#define MSR_IA32_TEMPERATURE_TARGET  0x1A2

static int thermal_state = -1;  /* -1 unprobed, 0 unsupported, 1 DTS */
static int thermal_tjmax_c = 100;
static unsigned int thermal_tjmax_raw;   /* last MSR 0x1A2 sample */
static int thermal_tjmax_msr;            /* TjMax came from the MSR */

static void cpuid_leaf(unsigned int leaf, unsigned int sub,
		       unsigned int *a, unsigned int *b, unsigned int *c,
		       unsigned int *d)
{
	__asm__ volatile ("cpuid"
			  : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			  : "a"(leaf), "c"(sub));
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int str_has(const char *hay, const char *needle)
{
	unsigned int i, j;

	for (i = 0; hay[i]; i++) {
		for (j = 0; needle[j] && hay[i + j] == needle[j]; j++)
			;
		if (!needle[j])
			return 1;
	}
	return 0;
}

/* Model allowlist from coretemp: only parts that really expose a DTS.  This
 * keeps rdmsr(0x19C) away from CPUs (and emulators) that set CPUID.06H but
 * do not implement the MSR, where it would raise #GP and panic. */
static int model_has_dts(unsigned int model)
{
	switch (model) {
	case 0x0E:   /* Core Solo/Duo (Yonah) */
	case 0x0F:   /* Core 2 */
	case 0x16:   /* Core 2 */
	case 0x17:   /* Penryn */
	case 0x1A:   /* Nehalem */
	case 0x1C:   /* Atom 230/330 (Diamondville) */
	case 0x1D:   /* Atom Pineview */
	case 0x1E:   /* Lynnfield */
	case 0x1F:   /* Nehalem */
	case 0x25:   /* Arrandale/Clarkdale (Core i5/i7 M, X201) */
	case 0x2C:   /* Westmere-EP */
	case 0x2E:   /* Nehalem-EX */
	case 0x2F:   /* Westmere-EX */
		return 1;
	default:
		return 0;
	}
}

/* Models whose TjMax must be read from IA32_TEMPERATURE_TARGET (0x1A2)
 * instead of the Bonnell table.  Nehalem and later implement the MSR; older
 * coretemp-listed parts do not, so a read there could raise #GP.  Linux's
 * coretemp reads the same MSR and falls back to 100 C when it is missing or
 * implausible. */
static int model_has_msr_tjmax(unsigned int model)
{
	switch (model) {
	case 0x1A:
	case 0x1E:
	case 0x1F:
	case 0x25:
	case 0x2C:
	case 0x2E:
	case 0x2F:
		return 1;
	default:
		return 0;
	}
}

static unsigned int read_tjmax_msr(void)
{
	unsigned int lo, hi;

	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi)
			  : "c"(MSR_IA32_TEMPERATURE_TARGET));
	(void)hi;
	return (lo >> 16) & 0xFF;
}

static void read_brand(char out[49])
{
	unsigned int max_ext, a, b, c, d, i;

	for (i = 0; i < 49; i++)
		out[i] = '\0';
	cpuid_leaf(0x80000000u, 0, &max_ext, &b, &c, &d);
	if (max_ext < 0x80000004u)
		return;
	for (i = 0; i < 3; i++) {
		unsigned int *dst = (unsigned int *)(out + i * 16);

		cpuid_leaf(0x80000002u + i, 0, &a, &b, &c, &d);
		dst[0] = a;
		dst[1] = b;
		dst[2] = c;
		dst[3] = d;
	}
}

static int pick_tjmax(unsigned int model, unsigned int stepping,
		      const char *brand)
{
	if (model == 0x1C || model == 0x1D) {
		/* The brand string often has double spaces ("CPU  330"), so
		 * match the number alone. */
		if (str_has(brand, "330"))
			return 125;
		if (str_has(brand, "230"))
			return 100;
		if (stepping == 10)     /* N4xx/N5xx/D4xx/D5xx */
			return 100;
		return 90;
	}
	return 100;
}

void thermal_init(void)
{
	unsigned int max_leaf, a, b, c, d;
	unsigned int family, model = 0, stepping = 0;
	char vendor[13];
	char brand[49];
	int msr_probed = 0;

	thermal_state = 0;
	thermal_tjmax_c = 100;
	if (!cpu_has_cpuid())
		goto out;

	cpuid_leaf(0, 0, &max_leaf, &b, &c, &d);
	((unsigned int *)vendor)[0] = b;
	((unsigned int *)vendor)[1] = d;
	((unsigned int *)vendor)[2] = c;
	vendor[12] = '\0';
	if (!str_eq(vendor, "GenuineIntel") || max_leaf < 1)
		goto out;

	cpuid_leaf(1, 0, &a, &b, &c, &d);
	if (!(d & (1u << 5)))          /* MSR support */
		goto out;
	family = (a >> 8) & 0xF;
	model = (a >> 4) & 0xF;
	stepping = a & 0xF;
	if (family == 0xF)
		family += (a >> 20) & 0xFF;
	if (family == 0x6 || family == 0xF)
		model += ((a >> 16) & 0xF) << 4;
	if (family != 0x6 || !model_has_dts(model))
		goto out;
	if (max_leaf < 6)
		goto out;
	cpuid_leaf(6, 0, &a, &b, &c, &d);
	if (!(a & 1u))                  /* DTS capability */
		goto out;

	read_brand(brand);
	thermal_tjmax_c = pick_tjmax(model, stepping, brand);
	thermal_tjmax_msr = 0;
	thermal_tjmax_raw = 0;
	if (model_has_msr_tjmax(model)) {
		unsigned int target = read_tjmax_msr();

		thermal_tjmax_raw = target;
		msr_probed = 1;
		if (target >= 80 && target <= 125) {
			thermal_tjmax_c = (int)target;
			thermal_tjmax_msr = 1;
		}
	}
	thermal_state = 1;

out:
	if (thermal_state) {
		serial_puts(SERIAL_COM1, "thermal: Intel DTS, model ");
		serial_putdec(SERIAL_COM1, model);
		serial_puts(SERIAL_COM1, " stepping ");
		serial_putdec(SERIAL_COM1, stepping);
		serial_puts(SERIAL_COM1, ", TjMax ");
		serial_putdec(SERIAL_COM1, (unsigned int)thermal_tjmax_c);
		serial_puts(SERIAL_COM1, " C");
		if (msr_probed) {
			serial_puts(SERIAL_COM1, " (msr 0x1A2=");
			serial_puthex(SERIAL_COM1, thermal_tjmax_raw);
			serial_puts(SERIAL_COM1,
				    thermal_tjmax_msr ? ")" : ", out of range)");
		} else {
			serial_puts(SERIAL_COM1, " (table)");
		}
		serial_puts(SERIAL_COM1, "\n");
	} else {
		serial_puts(SERIAL_COM1, "thermal: no Intel DTS, disabled\n");
	}
}

int thermal_read_this_cpu(void)
{
	unsigned int raw;
	int t;

	if (thermal_status_this_cpu(&raw) != 0)
		return -1;
	t = thermal_tjmax_c - (int)((raw >> 16) & 0x7F);
	if (t < -20 || t > 150)
		return -1;
	return t;
}

int thermal_status_this_cpu(unsigned int *status)
{
	unsigned int lo, hi;

	if (thermal_state < 0)
		thermal_init();
	if (!thermal_state)
		return -1;
	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi)
			  : "c"(MSR_IA32_THERM_STATUS));
	(void)hi;
	if (status)
		*status = lo;
	return 0;
}

void thermal_tick(void)
{
	struct cpu *c = this_cpu();
	unsigned int ms = (c->id == 0) ? timer_tick_ms() : c->tick_ms;

	/* The timer period drops to 10 ms while a CPU is idle, so count
	 * milliseconds rather than ticks to keep the 1 Hz sample rate. */
	if ((c->temp_div += ms) < 1000)
		return;
	c->temp_div = 0;
	c->temp_c = thermal_read_this_cpu();
}

int thermal_max_c(void)
{
	int max = -1;
	unsigned int i;

	if (thermal_state < 0)
		thermal_init();
	if (!thermal_state)
		return -1;
	for (i = 0; i < cpu_count && i < MAX_CPUS; i++)
		if (cpus[i].temp_c > max)
			max = cpus[i].temp_c;
	/* Before the first tick has published a value (or while a CPU is
	 * still booting), read the CPU running the caller instead of showing
	 * nothing for the first second. */
	if (max < 0)
		max = thermal_read_this_cpu();
	return max;
}

int thermal_tjmax(void)
{
	if (thermal_state < 0)
		thermal_init();
	return thermal_state ? thermal_tjmax_c : 0;
}
