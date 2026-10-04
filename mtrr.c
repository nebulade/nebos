/*
 * MTRR write-combining.
 *
 * The VESA LFB is mapped uncacheable (UC) by paging.c, so every dword the
 * 30 Hz shadow flush writes to VRAM is a separate uncached PCI transaction.
 * Marking the LFB range write-combining (WC) via an MTRR lets the CPU batch
 * those writes into burst transfers, which is the single biggest rendering
 * win on real hardware.  (The "P5 USWC" write-combining comment in vga_copy.S
 * does not hold on the Pentium 4 / Athlon class machines that ship with
 * Radeon RV280 cards.)
 */

#include "mtrr.h"
#include "serial.h"

#define MSR_MTRRCAP        0x0FE
#define MSR_MTRR_DEF_TYPE  0x2FF
#define MSR_MTRR_PHYSBASE0 0x200
#define MSR_MTRR_PHYSMASK0 0x201

#define MTRR_TYPE_WC       0x01

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

void mtrr_set_wc(unsigned int base, unsigned int size)
{
	unsigned int lo, hi;
	unsigned int def_type;
	unsigned int vcnt;
	unsigned int region_size, align;
	unsigned int physmask;

	if (!cpu_has_mtrr())
		return;
	if (size == 0)
		return;

	rdmsr(MSR_MTRRCAP, &lo, &hi);
	vcnt = lo & 0xFF;
	if (vcnt == 0)
		return;

	/* Round size up to the next power of two (min 4 KB). */
	region_size = 0x1000;
	while (region_size < size) {
		region_size <<= 1;
		if (region_size == 0)
			return;   /* overflow (size > 2 GB) -- give up */
	}

	/* A variable-range MTRR must be aligned to its own size.  The largest
	 * region we may use is therefore the base address's own power-of-two
	 * alignment (base & -base). */
	align = base & (~base + 1u);
	if (align == 0 || align > 0x80000000u)
		align = 0x80000000u;
	if (region_size > align)
		region_size = align;
	if (region_size < 0x1000)
		return;

	base &= ~(region_size - 1);

	/* PhysMask: address bits [11:0] are implicitly don't-care; bits
	 * [35:12] are the mask, with a 1 = don't-care.  For a <4 GB region the
	 * high 32-bit mask is 0 (bits 32..35 must match physbase == 0). */
	physmask = ((region_size - 1) & 0xFFFFF000u) | 0x800;

	/* Update MTRR with the global enable temporarily cleared so an
	 * in-progress region never exposes a bogus memory type. */
	rdmsr(MSR_MTRR_DEF_TYPE, &def_type, &hi);
	wrmsr(MSR_MTRR_DEF_TYPE, def_type & ~0x800u, 0);

	wrmsr(MSR_MTRR_PHYSBASE0, base | MTRR_TYPE_WC, 0);
	wrmsr(MSR_MTRR_PHYSMASK0, physmask, 0);

	wrmsr(MSR_MTRR_DEF_TYPE, def_type, 0);

	serial_puts(SERIAL_COM1, "mtrr: LFB WC [");
	serial_puthex(SERIAL_COM1, base);
	serial_puts(SERIAL_COM1, " +");
	serial_puthex(SERIAL_COM1, region_size);
	serial_puts(SERIAL_COM1, "]\n");
}
