/* Local APIC driver.  The MMIO window must already be mapped by smp.c. */

#include "lapic.h"
#include "timer.h"

#define LAPIC_ID        0x020
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define IA32_APIC_BASE_MSR 0x1B

static volatile unsigned int *lapic_mmio;
static unsigned int lapic_phys;
static unsigned int lapic_per_ms;

static inline unsigned int lapic_read(unsigned int off)
{
	return lapic_mmio[off / 4];
}

static inline void lapic_write(unsigned int off, unsigned int value)
{
	lapic_mmio[off / 4] = value;
}

static inline unsigned long long rdmsr(unsigned int msr)
{
	unsigned int lo, hi;
	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((unsigned long long)hi << 32) | lo;
}

static inline void wrmsr(unsigned int msr, unsigned long long value)
{
	unsigned int lo = (unsigned int)value;
	unsigned int hi = (unsigned int)(value >> 32);
	__asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void lapic_set_base(unsigned int base)
{
	lapic_phys = base;
	lapic_mmio = (volatile unsigned int *)base;
}

unsigned int lapic_base(void)
{
	return lapic_phys;
}

static void lapic_enable(void)
{
	unsigned long long msr = rdmsr(IA32_APIC_BASE_MSR);
	unsigned int base = (unsigned int)(msr & 0xFFFFF000u);
	unsigned int keep = (unsigned int)(msr & 0x100u); /* BSP bit */

	if (base != lapic_phys) {
		msr = (msr & ~0xFFFFF000ull) | lapic_phys;
		wrmsr(IA32_APIC_BASE_MSR, msr);
	}
	/* Keep the BSP bit, clear x2APIC (we drive the xAPIC MMIO window), and
	 * set the global enable bit. */
	msr = (msr & ~0xF00ull) | keep | 0x800ull;
	wrmsr(IA32_APIC_BASE_MSR, msr);
}

static int lapic_wait_icr(void)
{
	unsigned int n;

	for (n = 0; n < 1000000u; n++) {
		if (!(lapic_read(LAPIC_ICR_LO) & 0x1000u))
			return 1;
	}
	return 0;
}

void lapic_eoi(void)
{
	if (lapic_mmio)
		lapic_write(LAPIC_EOI, 0);
}

unsigned int lapic_id(void)
{
	return lapic_mmio ? (lapic_read(LAPIC_ID) >> 24) : 0xFFFFFFFFu;
}

int lapic_init_bsp(void)
{
	if (!lapic_mmio)
		return 0;
	lapic_enable();
	lapic_write(LAPIC_TPR, 0);
	/* Spurious vector: the handler must not send a PIC EOI. */
	lapic_write(LAPIC_SVR, 0x1FF);
	/* Keep the 8259 PIC on LINT0 (ExtINT); LINT1 stays NMI. */
	lapic_write(LAPIC_LVT_LINT0, 0x700);
	lapic_write(LAPIC_LVT_LINT1, 0x400);
	lapic_timer_stop();
	return 1;
}

int lapic_init_ap(void)
{
	if (!lapic_mmio)
		return 0;
	lapic_enable();
	lapic_write(LAPIC_TPR, 0);
	lapic_write(LAPIC_SVR, 0x1FF);
	lapic_write(LAPIC_LVT_LINT0, 0x10000); /* masked */
	lapic_write(LAPIC_LVT_LINT1, 0x10000);
	lapic_timer_stop();
	return 1;
}

void lapic_send_ipi(unsigned int apic_id, unsigned int vector)
{
	if (!lapic_mmio)
		return;
	lapic_wait_icr();
	lapic_write(LAPIC_ICR_HI, (apic_id & 0xFFu) << 24);
	lapic_write(LAPIC_ICR_LO, (vector & 0xFFu) | 0x00000000u);
	lapic_wait_icr();
}

void lapic_send_init(unsigned int apic_id)
{
	if (!lapic_mmio)
		return;
	lapic_wait_icr();
	lapic_write(LAPIC_ICR_HI, (apic_id & 0xFFu) << 24);
	/* delivery mode INIT (5), level assert. */
	lapic_write(LAPIC_ICR_LO, 0x00004500u);
	lapic_wait_icr();
}

void lapic_send_sipi(unsigned int apic_id, unsigned int vector)
{
	if (!lapic_mmio)
		return;
	lapic_wait_icr();
	lapic_write(LAPIC_ICR_HI, (apic_id & 0xFFu) << 24);
	/* delivery mode startup (6); vector is the real-mode page number. */
	lapic_write(LAPIC_ICR_LO, 0x00004600u | (vector & 0xFFu));
	lapic_wait_icr();
}

int lapic_timer_start(unsigned int vector)
{
	unsigned int t0, count, per_ms;

	if (!lapic_mmio)
		return 0;
	lapic_write(LAPIC_TIMER_DIV, 0x3); /* divide by 16 */
	lapic_write(LAPIC_LVT_TIMER, 0x10000); /* masked while calibrating */
	lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

	t0 = get_ticks_ms();
	while ((unsigned int)(get_ticks_ms() - t0) < 10)
		__asm__ volatile ("pause");
	count = lapic_read(LAPIC_TIMER_CUR);
	per_ms = (0xFFFFFFFFu - count) / 10u;
	if (per_ms == 0)
		return 0;

	lapic_write(LAPIC_LVT_TIMER, (vector & 0xFFu) | 0x00020000u);
	lapic_per_ms = per_ms;
	lapic_write(LAPIC_TIMER_INIT, per_ms);
	return 1;
}

void lapic_timer_set_ms(unsigned int ms)
{
	unsigned int init;

	if (!lapic_mmio || !lapic_per_ms)
		return;
	if (ms == 0)
		ms = 1;
	init = lapic_per_ms * ms;
	if (init)
		lapic_write(LAPIC_TIMER_INIT, init);
}

void lapic_timer_stop(void)
{
	if (lapic_mmio)
		lapic_write(LAPIC_LVT_TIMER, 0x10000);
}
