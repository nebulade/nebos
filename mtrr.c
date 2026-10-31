/*
 * MTRR write-combining.
 *
 * VRAM writes (the shadow flush) used to be separate uncached PCI
 * transactions: ~33 MB/s on the AT3IONT-I, so a full screen took 150 ms.
 * Marking the LFB range write-combining (WC) via an MTRR lets the CPU batch
 * those writes into bursts -- but only if the *pages* allow caching too:
 * paging.c used to map the LFB with PCD=1, which is UC regardless of the
 * MTRR, so the WC entry alone did nothing.  paging.c now installs the MTRR
 * first (through mtrr_set_wc(), which reports whether it covered the range)
 * and maps only the covered pages cacheable, leaving the rest UC.
 *
 * Two hard rules (the first key press froze an ASUS AT3IONT-I; these were
 * the only places where the kernel touched cacheability behind the BIOS's
 * back, and overwriting the BIOS's MMIO-cacheability entries is the prime
 * suspect, so both are fixed to be safe regardless):
 *
 *  1. NEVER overwrite an enabled BIOS MTRR entry.  Only ever add the WC
 *     range to a free variable entry; if none is free, skip WC entirely.
 *  2. Get the physmask direction right: mask bits are "compare" bits and
 *     are therefore the bitwise COMPLEMENT of (size - 1).  (The previous
 *     version had them inverted, so e.g. a 16 MB request marked the first
 *     4 KB of every 16 MB block as WC instead of the framebuffer range.)
 *
 * The MTRR layout found at boot and the result are dumped to the on-screen
 * debug lines (kdbg) and to serial, so this can be inspected on machines
 * without a serial console.
 */

#include "mtrr.h"
#include "kdbg.h"
#include "serial.h"

#define MSR_MTRRCAP        0x0FE
#define MSR_MTRR_DEF_TYPE  0x2FF
#define MSR_MTRR_PHYSBASE0 0x200
#define MSR_MTRR_PHYSMASK0 0x201

#define MTRR_TYPE_WC       0x01
#define MTRR_VALID         0x800

static void rdmsr(unsigned int msr, unsigned int *lo, unsigned int *hi)
{
	__asm__ volatile ("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static void wrmsr(unsigned int msr, unsigned int lo, unsigned int hi)
{
	__asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static int cpu_has_mtrr(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(1), "c"(0));
	return (edx & (1u << 12)) != 0;
}

/* Mask bits for physical address bits 32..35+.  Those are reserved (writing
 * 1 to an unimplemented bit faults) so ask CPUID for the real width.  All
 * ones for the implemented high bits means "must match PHYSBASE", which is
 * zero, so addresses >= 4 GB never match the WC entry. */
static unsigned int mtrr_hi_mask(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int bits, n;

	__asm__ volatile ("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(0x80000000u), "c"(0));
	if (eax < 0x80000008u)
		return 0;
	__asm__ volatile ("cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(0x80000008u), "c"(0));
	bits = eax & 0xFF;
	if (bits <= 32)
		return 0;
	n = bits - 32;
	if (n >= 32)
		return 0xFFFFFFFFu;
	return (1u << n) - 1;
}

static char *app(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *hex8(char *p, unsigned int v)
{
	static const char h[] = "0123456789ABCDEF";
	int i;
	for (i = 7; i >= 0; i--)
		*p++ = h[(v >> (i * 4)) & 0xF];
	return p;
}

static const char *type_name(unsigned int t)
{
	switch (t & 7) {
	case 0: return "UC";
	case 1: return "WC";
	case 4: return "WT";
	case 5: return "WP";
	case 6: return "WB";
	default: return "??";
	}
}

/* Dump MTRRdefType and the variable entries to kdbg and serial.  Rows come
 * from the kdbg info pool, so other subsystems keep their own. */
static void dump_mtrrs(unsigned int vcnt)
{
	char buf[56];
	char *p;
	unsigned int i, lo, hi, mlo, mhi;
	int row;

	rdmsr(MSR_MTRR_DEF_TYPE, &lo, &hi);
	p = buf;
	p = app(p, "MTRRdef ");
	p = hex8(p, lo);
	p = app(p, " vcnt ");
	*p++ = (char)('0' + (vcnt > 9 ? 9 : vcnt));
	*p = '\0';
	row = kdbg_info_alloc();
	if (row >= 0)
		kdbg_info((unsigned int)row, buf);
	serial_puts(SERIAL_COM1, buf);
	serial_puts(SERIAL_COM1, "\n");

	for (i = 0; i < vcnt && i < 8; i++) {
		rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i, &lo, &hi);
		rdmsr(MSR_MTRR_PHYSMASK0 + 2 * i, &mlo, &mhi);
		p = buf;
		*p++ = 'M';
		*p++ = (char)('0' + i);
		*p++ = ' ';
		if (!(mlo & MTRR_VALID)) {
			p = app(p, "-");
		} else {
			p = app(p, "b=");
			p = hex8(p, lo & 0xFFFFF000u);
			p = app(p, " m=");
			p = hex8(p, mlo & 0xFFFFF000u);
			*p++ = ' ';
			p = app(p, type_name(lo));
		}
		*p = '\0';
		row = kdbg_info_alloc();
		if (row >= 0)
			kdbg_info((unsigned int)row, buf);
		serial_puts(SERIAL_COM1, buf);
		serial_puts(SERIAL_COM1, "\n");
	}
}

int mtrr_set_wc(unsigned int base, unsigned int size,
		unsigned int *wc_base, unsigned int *wc_size)
{
	unsigned int lo, hi, mlo;
	unsigned int def_type;
	unsigned int vcnt;
	unsigned int i, use = ~0u;
	unsigned int region_size, align, physmask;
	int row;
	char buf[56];
	char *p;

	if (!cpu_has_mtrr() || size == 0)
		return 0;

	rdmsr(MSR_MTRRCAP, &lo, &hi);
	vcnt = lo & 0xFF;
	if (vcnt > 16)
		vcnt = 16;   /* sanity cap; CPUs ship 8..10 variable ranges */
	if (vcnt == 0)
		return 0;

	/* Dump the first 8 at most (kdbg info rows are a shared pool); the
	 * search below still covers every variable range. */
	dump_mtrrs(vcnt > 8 ? 8 : vcnt);

	/* Round size up to the next power of two (min 4 KB). */
	region_size = 0x1000;
	while (region_size < size) {
		region_size <<= 1;
		if (region_size == 0)
			return 0;   /* overflow (size > 2 GB) -- give up */
	}

	/* A variable-range MTRR must be aligned to its own size.  The largest
	 * region we may use is therefore the base address's own power-of-two
	 * alignment. */
	align = base & (~base + 1u);
	if (align == 0 || align > 0x80000000u)
		align = 0x80000000u;
	if (region_size > align)
		region_size = align;
	if (region_size < 0x1000)
		return 0;
	base &= ~(region_size - 1);

	/* A kernel reload does not reset the MTRRs: reuse the WC entry an
	 * earlier boot left for this base instead of burning another variable
	 * slot (the kexec dev loop would otherwise exhaust the entries). */
	for (i = 0; i < vcnt; i++) {
		unsigned int esize;

		rdmsr(MSR_MTRR_PHYSBASE0 + 2 * i, &lo, &hi);
		rdmsr(MSR_MTRR_PHYSMASK0 + 2 * i, &mlo, &hi);
		if (!(mlo & MTRR_VALID) || (lo & 0xFF) != MTRR_TYPE_WC ||
		    (lo & 0xFFFFF000u) != base)
			continue;
		esize = ((~mlo) & 0xFFFFF000u) + 0x1000;
		if (esize < region_size)
			continue;
		serial_puts(SERIAL_COM1, "mtrr: LFB already write-combining\n");
		if (wc_base)
			*wc_base = base;
		if (wc_size)
			*wc_size = esize;
		return 1;
	}

	/* Only ever use a free variable entry.  BIOS-populated entries may
	 * keep chipset MMIO uncached and must not be disturbed. */
	for (i = 0; i < vcnt; i++) {
		rdmsr(MSR_MTRR_PHYSMASK0 + 2 * i, &mlo, &hi);
		if (!(mlo & MTRR_VALID)) {
			use = i;
			break;
		}
	}

	p = buf;
	if (use == ~0u) {
		p = app(p, "WC skipped: no free MTRR entry");
		*p = '\0';
		row = kdbg_info_alloc();
		if (row >= 0)
			kdbg_info((unsigned int)row, buf);
		serial_puts(SERIAL_COM1, "mtrr: no free variable entry; skipping WC\n");
		return 0;
	}

	/* PhysMask bits [35:12] are compare bits: 1 = the address bit must
	 * match PHYSBASE.  For a 2^n-sized region, bits [n-1:12] are
	 * don't-care (0) and every higher bit must match (1), i.e. the
	 * complement of (size - 1). */
	physmask = (~(region_size - 1) & 0xFFFFF000u) | MTRR_VALID;

	/* Update the entry with the global enable temporarily cleared so an
	 * in-progress region never exposes a bogus memory type. */
	rdmsr(MSR_MTRR_DEF_TYPE, &def_type, &hi);
	wrmsr(MSR_MTRR_DEF_TYPE, def_type & ~MTRR_VALID, 0);

	wrmsr(MSR_MTRR_PHYSBASE0 + 2 * use, base | MTRR_TYPE_WC, 0);
	wrmsr(MSR_MTRR_PHYSMASK0 + 2 * use, physmask, mtrr_hi_mask());

	wrmsr(MSR_MTRR_DEF_TYPE, def_type, 0);

	p = app(p, "WC=e");
	*p++ = (char)('0' + (use > 9 ? 9 : use));
	p = app(p, " b=");
	p = hex8(p, base);
	p = app(p, " s=");
	p = hex8(p, region_size);
	*p = '\0';
	row = kdbg_info_alloc();
	if (row >= 0)
		kdbg_info((unsigned int)row, buf);

	serial_puts(SERIAL_COM1, "mtrr: LFB WC [");
	serial_puthex(SERIAL_COM1, base);
	serial_puts(SERIAL_COM1, " +");
	serial_puthex(SERIAL_COM1, region_size);
	serial_puts(SERIAL_COM1, "]\n");

	if (wc_base)
		*wc_base = base;
	if (wc_size)
		*wc_size = region_size;
	return 1;
}
