/* cpuinfo command: print CPU vendor/model and estimated current clock speed. */

#include "idle.h"
#include "io.h"
#include "timer.h"

#define MSR_IA32_PERF_STATUS     0x198u
#define MSR_IA32_PERF_CTL        0x199u
#define MSR_IA32_PLATFORM_INFO   0xCEu
#define MSR_IA32_PM_ENABLE       0x770u
#define MSR_IA32_HWP_CAPABILITIES 0x771u
#define MSR_IA32_HWP_REQUEST     0x774u
#define MSR_IA32_ENERGY_PERF_BIAS 0x1B0u
#define MSR_AMD_PSTATE_CTL       0xC0010062u
#define MSR_AMD_PSTATE_STATUS    0xC0010063u

static void put_uint(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;
	*p = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	while (*p)
		io_putchar(*p++);
}

static int streq(const char *a, const char *b)
{
	unsigned int i = 0;
	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

static int str_starts_with(const char *s, const char *prefix)
{
	unsigned int i = 0;
	while (prefix[i]) {
		if (s[i] != prefix[i])
			return 0;
		i++;
	}
	return 1;
}

static void put_hex8(unsigned int x)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(x >> 4) & 0xF]);
	io_putchar(hex[x & 0xF]);
}

static void put_hex32(unsigned int x)
{
	put_hex8((x >> 24) & 0xFF);
	put_hex8((x >> 16) & 0xFF);
	put_hex8((x >> 8) & 0xFF);
	put_hex8(x & 0xFF);
}

static void cpuid(unsigned int leaf, unsigned int subleaf,
	unsigned int *eax, unsigned int *ebx,
	unsigned int *ecx, unsigned int *edx)
{
	__asm__ volatile ("cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf), "c"(subleaf));
}

static unsigned int read_tsc_low(void)
{
	unsigned int lo, hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	(void)hi;
	return lo;
}

static void rdmsr_u32(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
	__asm__ volatile ("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static void wrmsr_u32(unsigned int msr, unsigned int lo, unsigned int hi)
{
	__asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static void trim_right_spaces(char *s)
{
	unsigned int len = 0;
	while (s[len])
		len++;
	while (len > 0 && s[len - 1] == ' ') {
		s[len - 1] = '\0';
		len--;
	}
}

static void print_flag(const char *name, int *printed_any)
{
	if (*printed_any)
		io_putchar(' ');
	io_putstr(name);
	*printed_any = 1;
}

static void print_features_leaf1(unsigned int ecx1, unsigned int edx1)
{
	int any = 0;
	io_putstr("ext l1 ");
	if (edx1 & (1u << 0))  print_flag("FPU", &any);
	if (edx1 & (1u << 4))  print_flag("TSC", &any);
	if (edx1 & (1u << 5))  print_flag("MSR", &any);
	if (edx1 & (1u << 6))  print_flag("PAE", &any);
	if (edx1 & (1u << 9))  print_flag("APIC", &any);
	if (edx1 & (1u << 12)) print_flag("MTRR", &any);
	if (edx1 & (1u << 15)) print_flag("CMOV", &any);
	if (edx1 & (1u << 16)) print_flag("PAT", &any);
	if (edx1 & (1u << 17)) print_flag("PSE36", &any);
	if (edx1 & (1u << 23)) print_flag("MMX", &any);
	if (edx1 & (1u << 24)) print_flag("FXSR", &any);
	if (edx1 & (1u << 25)) print_flag("SSE", &any);
	if (edx1 & (1u << 26)) print_flag("SSE2", &any);
	if (edx1 & (1u << 28)) print_flag("HTT", &any);
	if (ecx1 & (1u << 0))  print_flag("SSE3", &any);
	if (ecx1 & (1u << 1))  print_flag("PCLMULQDQ", &any);
	if (ecx1 & (1u << 9))  print_flag("SSSE3", &any);
	if (ecx1 & (1u << 12)) print_flag("FMA", &any);
	if (ecx1 & (1u << 13)) print_flag("CX16", &any);
	if (ecx1 & (1u << 19)) print_flag("SSE4.1", &any);
	if (ecx1 & (1u << 20)) print_flag("SSE4.2", &any);
	if (ecx1 & (1u << 22)) print_flag("MOVBE", &any);
	if (ecx1 & (1u << 23)) print_flag("POPCNT", &any);
	if (ecx1 & (1u << 25)) print_flag("AES", &any);
	if (ecx1 & (1u << 26)) print_flag("XSAVE", &any);
	if (ecx1 & (1u << 27)) print_flag("OSXSAVE", &any);
	if (ecx1 & (1u << 28)) print_flag("AVX", &any);
	if (ecx1 & (1u << 29)) print_flag("F16C", &any);
	if (ecx1 & (1u << 30)) print_flag("RDRAND", &any);
	if (!any)
		io_putstr("none");
	io_putchar('\n');
}

static void print_features_leaf7(unsigned int max_leaf)
{
	unsigned int eax7, ebx7, ecx7, edx7;
	int any = 0;
	if (max_leaf < 7)
		return;
	cpuid(7, 0, &eax7, &ebx7, &ecx7, &edx7);
	(void)eax7;
	(void)ecx7;
	(void)edx7;
	io_putstr("ext l7 ");
	if (ebx7 & (1u << 0))  print_flag("FSGSBASE", &any);
	if (ebx7 & (1u << 3))  print_flag("BMI1", &any);
	if (ebx7 & (1u << 5))  print_flag("AVX2", &any);
	if (ebx7 & (1u << 8))  print_flag("BMI2", &any);
	if (ebx7 & (1u << 9))  print_flag("ERMS", &any);
	if (ebx7 & (1u << 18)) print_flag("RDSEED", &any);
	if (ebx7 & (1u << 19)) print_flag("ADX", &any);
	if (ebx7 & (1u << 20)) print_flag("SMAP", &any);
	if (ebx7 & (1u << 23)) print_flag("CLFLUSHOPT", &any);
	if (ebx7 & (1u << 29)) print_flag("SHA", &any);
	if (!any)
		io_putstr("none");
	io_putchar('\n');
}

static void print_features_ext(unsigned int max_ext_leaf)
{
	unsigned int eax, ebx, ecx, edx;
	int any = 0;
	if (max_ext_leaf < 0x80000001u)
		return;
	cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
	(void)eax;
	(void)ebx;
	io_putstr("ext x ");
	if (edx & (1u << 11)) print_flag("SYSCALL", &any);
	if (edx & (1u << 20)) print_flag("NX", &any);
	if (edx & (1u << 27)) print_flag("RDTSCP", &any);
	if (edx & (1u << 29)) print_flag("LM", &any);
	if (ecx & (1u << 0))  print_flag("LAHF_LM", &any);
	if (ecx & (1u << 2))  print_flag("SVM", &any);
	if (ecx & (1u << 5))  print_flag("ABM", &any);
	if (ecx & (1u << 6))  print_flag("SSE4A", &any);
	if (ecx & (1u << 8))  print_flag("PREFETCHW", &any);
	if (!any)
		io_putstr("none");
	io_putchar('\n');
}

static void print_cpuid_frequency(unsigned int max_leaf)
{
	unsigned int eax, ebx, ecx, edx;

	if (max_leaf >= 0x15u) {
		cpuid(0x15u, 0, &eax, &ebx, &ecx, &edx);
		if (eax != 0 && ebx != 0) {
			io_putstr("cpuid15 ratio ");
			put_uint(ebx);
			io_putchar('/');
			put_uint(eax);
			if (ecx != 0) {
				io_putstr(" crystal ");
				put_uint(ecx / 1000000u);
				io_putstr(" MHz");
			}
			io_putchar('\n');
		}
	}
	if (max_leaf >= 0x16u) {
		cpuid(0x16u, 0, &eax, &ebx, &ecx, &edx);
		if (eax != 0 || ebx != 0 || ecx != 0) {
			io_putstr("cpuid16 base ");
			put_uint(eax);
			io_putstr(" max ");
			put_uint(ebx);
			io_putstr(" bus ");
			put_uint(ecx);
			io_putstr(" MHz\n");
		}
	}
}

static void parse_family_model(unsigned int eax1,
			       unsigned int *family_out,
			       unsigned int *model_out,
			       unsigned int *stepping_out)
{
	unsigned int family = (eax1 >> 8) & 0xF;
	unsigned int model = (eax1 >> 4) & 0xF;
	unsigned int stepping = eax1 & 0xF;
	if (family == 0xF)
		family += (eax1 >> 20) & 0xFF;
	if (family == 0x6 || family == 0xF)
		model += ((eax1 >> 16) & 0xF) << 4;
	*family_out = family;
	*model_out = model;
	*stepping_out = stepping;
}

void program_cpuinfo_main(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int ecx1, edx1, ebx1;
	char vendor[13];
	char brand[49];
	unsigned int max_leaf;
	unsigned int max_ext_leaf;
	unsigned int family, model, stepping;
	unsigned int logical_cpus = 1;
	unsigned int cores = 1;
	unsigned int threads_per_core = 1;
	unsigned int start_ms, end_ms, delta_ms;
	unsigned int t0, t1, delta_cycles;
	unsigned int mhz_x10;
	unsigned int has_brand = 0;
	unsigned int i;

	cpuid(0, 0, &max_leaf, &ebx, &ecx, &edx);
	((unsigned int *)vendor)[0] = ebx;
	((unsigned int *)vendor)[1] = edx;
	((unsigned int *)vendor)[2] = ecx;
	vendor[12] = '\0';
	cpuid(0x80000000u, 0, &max_ext_leaf, &ebx, &ecx, &edx);

	io_putstr("vendor ");
	io_putstr(vendor);
	io_putchar('\n');
	io_putstr("cpuid max ");
	put_uint(max_leaf);
	io_putstr(" ext max ");
	put_uint(max_ext_leaf & 0xFFFFu);
	io_putchar('\n');

	cpuid(1, 0, &eax, &ebx, &ecx, &edx);
	ebx1 = ebx;
	ecx1 = ecx;
	edx1 = edx;
	parse_family_model(eax, &family, &model, &stepping);

	io_putstr("family ");
	put_uint(family);
	io_putstr(" model ");
	put_uint(model);
	io_putstr(" stepping ");
	put_uint(stepping);
	io_putchar('\n');

	if (max_ext_leaf >= 0x80000004u) {
		for (i = 0; i < 48; i++)
			brand[i] = '\0';
		for (i = 0; i < 3; i++) {
			unsigned int *dst = (unsigned int *)(brand + i * 16);
			cpuid(0x80000002u + i, 0, &eax, &ebx, &ecx, &edx);
			dst[0] = eax;
			dst[1] = ebx;
			dst[2] = ecx;
			dst[3] = edx;
		}
		brand[48] = '\0';
		trim_right_spaces(brand);
		if (brand[0] != '\0')
			has_brand = 1;
	}
	if (has_brand) {
		io_putstr("brand ");
		io_putstr(brand);
		io_putchar('\n');
	}

	/* Logical processors package-wide from leaf 1 EBX[23:16]. */
	logical_cpus = (ebx1 >> 16) & 0xFF;
	if (logical_cpus == 0)
		logical_cpus = 1;

	/* Physical core count (best effort across vendors/generations). */
	if (max_leaf >= 4) {
		cpuid(4, 0, &eax, &ebx, &ecx, &edx);
		cores = ((eax >> 26) & 0x3F) + 1;
	}
	if (max_ext_leaf >= 0x80000008u) {
		cpuid(0x80000008u, 0, &eax, &ebx, &ecx, &edx);
		if ((ecx & 0xFFu) != 0)
			cores = (ecx & 0xFFu) + 1;
	}
	if (cores == 0)
		cores = 1;
	if (logical_cpus >= cores && cores != 0)
		threads_per_core = logical_cpus / cores;
	if (threads_per_core == 0)
		threads_per_core = 1;

	io_putstr("topology ");
	put_uint(logical_cpus);
	io_putstr(" logical, ");
	put_uint(cores);
	io_putstr(" cores, ");
	put_uint(threads_per_core);
	io_putstr(" threads/core\n");

	print_cpuid_frequency(max_leaf);

	start_ms = get_ticks_ms();
	t0 = read_tsc_low();
	do {
		__asm__ volatile ("hlt");
		end_ms = get_ticks_ms();
	} while (end_ms - start_ms < 200);
	t1 = read_tsc_low();
	delta_ms = end_ms - start_ms;
	delta_cycles = t1 - t0;
	if (delta_ms == 0)
		delta_ms = 1;

	/* MHz with one decimal: cycles / (ms * 100). */
	mhz_x10 = delta_cycles / (delta_ms * 100);
	io_putstr("clock tsc ");
	put_uint(mhz_x10 / 10);
	io_putchar('.');
	put_uint(mhz_x10 % 10);
	io_putstr(" MHz (sampled)\n");

	print_features_leaf1(ecx1, edx1);
	print_features_leaf7(max_leaf);
	print_features_ext(max_ext_leaf);
}

static void print_power_usage(void)
{
	io_putstr("usage:\n");
	io_putstr("  power info\n");
	io_putstr("  power set low      (deep C-state, lowest power)\n");
	io_putstr("  power set balanced (moderate C-state)\n");
	io_putstr("  power set max      (C1 only, max performance)\n");
}

static void detect_cpu(char vendor[13], unsigned int *max_leaf,
		       unsigned int *eax1, unsigned int *ecx1, unsigned int *edx1,
		       unsigned int *family, unsigned int *model, unsigned int *stepping)
{
	unsigned int ebx, ecx, edx;
	cpuid(0, 0, max_leaf, &ebx, &ecx, &edx);
	((unsigned int *)vendor)[0] = ebx;
	((unsigned int *)vendor)[1] = edx;
	((unsigned int *)vendor)[2] = ecx;
	vendor[12] = '\0';
	cpuid(1, 0, eax1, &ebx, ecx1, edx1);
	parse_family_model(*eax1, family, model, stepping);
}

static int intel_has_platform_info(unsigned int family, unsigned int model)
{
	if (family != 6)
		return 0;
	if (model >= 0x1A)
		return 1;
	return 0;
}

static int intel_has_epb(unsigned int eax6)
{
	return (eax6 & (1u << 3)) ? 1 : 0;
}

static void power_info_intel(unsigned int family, unsigned int model, unsigned int ecx1, unsigned int edx1)
{
	unsigned int lo = 0, hi = 0;
	unsigned int target_ratio = 0, current_ratio = 0;
	unsigned int min_ratio = 0, max_ratio = 0;
	unsigned int eax6, ebx6, ecx6, edx6;
	int has_hwp = 0;
	int hwp_enabled = 0;
	int has_platinfo = 0;
	int has_epb = 0;

	if ((edx1 & (1u << 5)) == 0) {
		io_putstr("intel: no MSR feature bit\n");
		return;
	}
	if ((ecx1 & (1u << 7)) == 0)
		io_putstr("intel: EIST not advertised\n");
	if (family != 6 && family != 15) {
		io_putstr("intel: family not supported for perf ctl\n");
		return;
	}

	cpuid(6, 0, &eax6, &ebx6, &ecx6, &edx6);
	(void)ebx6;
	(void)ecx6;
	(void)edx6;
	has_hwp = (eax6 & (1u << 7)) ? 1 : 0;
	has_epb = intel_has_epb(eax6);
	has_platinfo = intel_has_platform_info(family, model);

	if (has_platinfo) {
		rdmsr_u32(MSR_IA32_PLATFORM_INFO, &lo, &hi);
		min_ratio = (hi >> 8) & 0xFFu;
		max_ratio = (lo >> 8) & 0xFFu;
	}

	rdmsr_u32(MSR_IA32_PERF_STATUS, &lo, &hi);
	current_ratio = (lo >> 8) & 0xFF;
	rdmsr_u32(MSR_IA32_PERF_CTL, &lo, &hi);
	target_ratio = (lo >> 8) & 0xFF;

	if (has_hwp) {
		rdmsr_u32(MSR_IA32_PM_ENABLE, &lo, &hi);
		hwp_enabled = (lo & 1u) ? 1 : 0;
	}

	io_putstr("intel ratio current ");
	put_uint(current_ratio);
	io_putstr(" target ");
	put_uint(target_ratio);
	io_putchar('\n');
	if (has_platinfo) {
		io_putstr("intel ratio range ");
		put_uint(min_ratio);
		io_putstr("..");
		put_uint(max_ratio);
		io_putchar('\n');
	}
	if (has_hwp) {
		io_putstr("intel hwp ");
		io_putstr(hwp_enabled ? "enabled\n" : "disabled\n");
		if (hwp_enabled) {
			rdmsr_u32(MSR_IA32_HWP_REQUEST, &lo, &hi);
			io_putstr("intel hwp req min ");
			put_uint(lo & 0xFFu);
			io_putstr(" max ");
			put_uint((lo >> 8) & 0xFFu);
			io_putstr(" des ");
			put_uint((lo >> 16) & 0xFFu);
			io_putstr(" epp ");
			put_uint((lo >> 24) & 0xFFu);
			io_putchar('\n');
			rdmsr_u32(MSR_IA32_HWP_CAPABILITIES, &lo, &hi);
			io_putstr("intel hwp cap low ");
			put_uint(lo & 0xFFu);
			io_putstr(" high ");
			put_uint((lo >> 8) & 0xFFu);
			io_putstr(" guar ");
			put_uint((lo >> 16) & 0xFFu);
			io_putstr(" eff ");
			put_uint((lo >> 24) & 0xFFu);
			io_putchar('\n');
		}
	}
	if (has_epb) {
		rdmsr_u32(MSR_IA32_ENERGY_PERF_BIAS, &lo, &hi);
		io_putstr("intel epb ");
		put_uint(lo & 0xFu);
		io_putchar('\n');
	}
	io_putstr("intel family ");
	put_uint(family);
	io_putstr(" model ");
	put_uint(model);
	io_putchar('\n');
}

static int power_set_intel(const char *profile, unsigned int family, unsigned int model, unsigned int ecx1, unsigned int edx1)
{
	unsigned int lo = 0, hi = 0;
	unsigned int current_ratio, target_ratio;
	unsigned int min_ratio = 8, max_ratio = 63;
	unsigned int eax6, ebx6, ecx6, edx6;
	int has_hwp = 0;
	int hwp_enabled = 0;
	int has_platinfo = 0;

	if ((edx1 & (1u << 5)) == 0 || (ecx1 & (1u << 7)) == 0) {
		io_putstr("intel: missing MSR/EIST support\n");
		return -1;
	}
	if (family != 6 && family != 15) {
		io_putstr("intel: unsupported family for write path\n");
		return -1;
	}

	cpuid(6, 0, &eax6, &ebx6, &ecx6, &edx6);
	(void)ebx6;
	(void)ecx6;
	(void)edx6;
	has_hwp = (eax6 & (1u << 7)) ? 1 : 0;
	has_platinfo = intel_has_platform_info(family, model);

	if (has_platinfo) {
		rdmsr_u32(MSR_IA32_PLATFORM_INFO, &lo, &hi);
		min_ratio = (hi >> 8) & 0xFFu;
		max_ratio = (lo >> 8) & 0xFFu;
		if (min_ratio == 0 || max_ratio == 0 || min_ratio > max_ratio) {
			min_ratio = 8;
			max_ratio = 63;
		}
	}

	if (has_hwp) {
		rdmsr_u32(MSR_IA32_PM_ENABLE, &lo, &hi);
		hwp_enabled = (lo & 1u) ? 1 : 0;
	}

	rdmsr_u32(MSR_IA32_PERF_STATUS, &lo, &hi);
	current_ratio = (lo >> 8) & 0xFF;
	target_ratio = current_ratio < min_ratio ? min_ratio : current_ratio;
	if (target_ratio > max_ratio)
		target_ratio = max_ratio;

	if (streq(profile, "low")) {
		target_ratio = min_ratio;
	} else if (streq(profile, "balanced")) {
		target_ratio = (min_ratio + max_ratio) / 2;
	} else if (streq(profile, "max")) {
		target_ratio = max_ratio;
	} else {
		io_putstr("power: invalid profile\n");
		return -1;
	}

	if (has_hwp && hwp_enabled) {
		unsigned int minp = min_ratio;
		unsigned int maxp = max_ratio;
		unsigned int des = target_ratio;
		unsigned int epp = 0x80u;
		if (streq(profile, "low"))
			epp = 0xC0u;
		else if (streq(profile, "balanced"))
			epp = 0x80u;
		else if (streq(profile, "max"))
			epp = 0x00u;
		lo = (minp & 0xFFu) |
		     ((maxp & 0xFFu) << 8) |
		     ((des & 0xFFu) << 16) |
		     ((epp & 0xFFu) << 24);
		wrmsr_u32(MSR_IA32_HWP_REQUEST, lo, 0);
		io_putstr("intel hwp profile ");
		io_putstr(profile);
		io_putstr(" set (min ");
		put_uint(minp);
		io_putstr(" max ");
		put_uint(maxp);
		io_putstr(" des ");
		put_uint(des);
		io_putstr(" epp ");
		put_uint(epp);
		io_putstr(")\n");
		return 0;
	}

	rdmsr_u32(MSR_IA32_PERF_CTL, &lo, &hi);
	lo &= ~0xFF00u;
	lo |= (target_ratio & 0xFFu) << 8;
	wrmsr_u32(MSR_IA32_PERF_CTL, lo, hi);
	io_putstr("intel target ratio set ");
	put_uint(target_ratio);
	io_putchar('\n');
	return 0;
}

static void power_info_amd(unsigned int family, unsigned int model, unsigned int edx1)
{
	unsigned int lo = 0, hi = 0;
	if ((edx1 & (1u << 5)) == 0) {
		io_putstr("amd: no MSR feature bit\n");
		return;
	}
	if (family < 0x10) {
		io_putstr("amd: family too old for p-state ctl\n");
		return;
	}
	rdmsr_u32(MSR_AMD_PSTATE_STATUS, &lo, &hi);
	io_putstr("amd pstate current ");
	put_uint(lo & 0x7u);
	io_putstr(" raw 0x");
	put_hex32(lo);
	io_putchar('\n');
	io_putstr("amd family ");
	put_uint(family);
	io_putstr(" model ");
	put_uint(model);
	io_putchar('\n');
}

static int power_set_amd(const char *profile, unsigned int family, unsigned int edx1)
{
	unsigned int lo = 0, hi = 0;
	unsigned int pstate = 0;
	if ((edx1 & (1u << 5)) == 0) {
		io_putstr("amd: no MSR support\n");
		return -1;
	}
	if (family < 0x10) {
		io_putstr("amd: unsupported family for write path\n");
		return -1;
	}
	if (streq(profile, "max")) {
		pstate = 0;
	} else if (streq(profile, "balanced")) {
		pstate = 1;
	} else if (streq(profile, "low")) {
		pstate = 2;
	} else {
		io_putstr("power: invalid profile\n");
		return -1;
	}
	rdmsr_u32(MSR_AMD_PSTATE_CTL, &lo, &hi);
	lo &= ~0x7u;
	lo |= pstate;
	wrmsr_u32(MSR_AMD_PSTATE_CTL, lo, hi);
	io_putstr("amd target pstate set ");
	put_uint(pstate);
	io_putchar('\n');
	return 0;
}

void program_power_main(const char *args)
{
	char vendor[13];
	char local[40];
	char command[8];
	char profile[16];
	unsigned int max_leaf, eax1, ecx1, edx1, family, model, stepping;
	unsigned int i = 0, j = 0;
	int is_set = 0;
	unsigned int cstate_level;

	detect_cpu(vendor, &max_leaf, &eax1, &ecx1, &edx1, &family, &model, &stepping);
	(void)eax1;
	(void)max_leaf;

	while (args && args[i] && i + 1 < sizeof(local)) {
		local[i] = args[i];
		i++;
	}
	local[i] = '\0';
	i = 0;
	while (local[i] == ' ')
		i++;
	j = 0;
	while (local[i] && local[i] != ' ' && j + 1 < sizeof(command))
		command[j++] = local[i++];
	command[j] = '\0';
	while (local[i] == ' ')
		i++;
	j = 0;
	while (local[i] && local[i] != ' ' && j + 1 < sizeof(profile))
		profile[j++] = local[i++];
	profile[j] = '\0';

	if (command[0] == '\0' || streq(command, "info")) {
		io_putstr("power vendor ");
		io_putstr(vendor);
		io_putstr(" family ");
		put_uint(family);
		io_putstr(" model ");
		put_uint(model);
		io_putstr(" stepping ");
		put_uint(stepping);
		io_putchar('\n');

		io_putstr("idle mode ");
		if (idle_has_mwait())
			io_putstr("mwait");
		else
			io_putstr("hlt");
		io_putstr(" cstate C");
		put_uint(idle_get_cstate());
		io_putchar('\n');

		if (streq(vendor, "GenuineIntel"))
			power_info_intel(family, model, ecx1, edx1);
		else if (streq(vendor, "AuthenticAMD"))
			power_info_amd(family, model, edx1);
		else
			io_putstr("power: vendor backend unavailable\n");
		print_power_usage();
		return;
	}

	if (streq(command, "set"))
		is_set = 1;
	if (!is_set || profile[0] == '\0') {
		print_power_usage();
		return;
	}
	if (str_starts_with(profile, "low") == 0 &&
	    str_starts_with(profile, "balanced") == 0 &&
	    str_starts_with(profile, "max") == 0) {
		io_putstr("power: expected low|balanced|max\n");
		return;
	}

	if (streq(profile, "low")) {
		cstate_level = 3;
	} else if (streq(profile, "balanced")) {
		cstate_level = 2;
	} else {
		cstate_level = 0;
	}
	idle_set_cstate(cstate_level);
	io_putstr("idle cstate set C");
	put_uint(cstate_level);
	io_putchar('\n');

	if (streq(vendor, "GenuineIntel")) {
		if (power_set_intel(profile, family, model, ecx1, edx1) < 0)
			io_putstr("power: intel p-state control unavailable\n");
	} else if (streq(vendor, "AuthenticAMD")) {
		if (power_set_amd(profile, family, edx1) < 0)
			io_putstr("power: amd p-state control unavailable\n");
	}
}
