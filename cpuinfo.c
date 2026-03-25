/* cpuinfo command: print CPU vendor/model and estimated current clock speed. */

#include "io.h"
#include "timer.h"

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
	stepping = eax & 0xF;
	model = (eax >> 4) & 0xF;
	family = (eax >> 8) & 0xF;
	if (family == 0xF)
		family += (eax >> 20) & 0xFF;
	if (family == 0x6 || family == 0xF)
		model += ((eax >> 16) & 0xF) << 4;

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
	io_putstr("clock ");
	put_uint(mhz_x10 / 10);
	io_putchar('.');
	put_uint(mhz_x10 % 10);
	io_putstr(" MHz (estimated)\n");

	print_features_leaf1(ecx1, edx1);
	print_features_leaf7(max_leaf);
	print_features_ext(max_ext_leaf);
}
