/* SMP bring-up: ACPI CPU list, AP trampoline, INIT-SIPI and per-CPU idle.
 *
 * No MP-table fallback and no IOAPIC: this targets ACPI/MADT machines (QEMU
 * and the Atom board).  If ACPI is absent or has one CPU, smp_init() leaves
 * the system exactly as it was.
 */

#include "smp.h"
#include "acpi.h"
#include "cpu.h"
#include "idle.h"
#include "lapic.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "serial.h"
#include "timer.h"
#include "tss.h"

#define AP_TRAMP_ADDR 0x6000u
#define AP_INFO_ADDR  0x6F00u

extern unsigned char _binary_ap_tramp_bin_start[];
extern unsigned char _binary_ap_tramp_bin_end[];
extern void idt_load_ap(void);

static volatile unsigned int ap_started[MAX_CPUS];
static unsigned int online_aps;
unsigned int smp_sched_enabled;

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while ((unsigned int)(get_ticks_ms() - t0) < ms)
		cpu_idle();
}

static int copy_trampoline(void)
{
	unsigned int n = (unsigned int)(_binary_ap_tramp_bin_end -
					_binary_ap_tramp_bin_start);
	unsigned char *dst = (unsigned char *)AP_TRAMP_ADDR;
	unsigned int i;

	if (n == 0 || n > 0xC00)
		return 0;
	for (i = 0; i < n; i++)
		dst[i] = _binary_ap_tramp_bin_start[i];
	__asm__ volatile ("wbinvd" ::: "memory");
	return 1;
}

static void start_one_ap(unsigned int cpu, unsigned int top)
{
	volatile unsigned int *info = (volatile unsigned int *)AP_INFO_ADDR;
	unsigned int apic = acpi_cpu_apic_id(cpu);
	unsigned int t0;

	info[0] = paging_kernel_cr3();
	info[1] = top;
	info[2] = (unsigned int)smp_ap_main;
	info[3] = cpu;
	__sync_synchronize();

	ap_started[cpu] = 0;
	lapic_send_init(apic);
	delay_ms(10);
	lapic_send_sipi(apic, AP_TRAMP_ADDR >> 12);

	/* The spec recommends a second SIPI if the first one was missed; some
	 * firmware/boards drop it while the AP is still finishing INIT. */
	t0 = get_ticks_ms();
	while ((unsigned int)(get_ticks_ms() - t0) < 10) {
		if (ap_started[cpu])
			return;
		cpu_idle();
	}
	if (!ap_started[cpu])
		lapic_send_sipi(apic, AP_TRAMP_ADDR >> 12);

	t0 = get_ticks_ms();
	while ((unsigned int)(get_ticks_ms() - t0) < 500) {
		if (ap_started[cpu])
			return;
		cpu_idle();
	}

	/* It did not check in; don't let it start later with stale info. */
	lapic_send_init(apic);
}

void smp_init(void)
{
	unsigned int n = acpi_cpu_count();
	unsigned int i;

	online_aps = 0;
	if (n <= 1) {
		serial_puts(SERIAL_COM1, "smp: unavailable, 1 cpu\n");
		return;
	}
	if (!acpi_lapic_base())
		return;

	lapic_set_base(acpi_lapic_base());
	paging_map_mmio(acpi_lapic_base(), 0x1000);
	/* The 4 MB MMIO page could share a PDE with the framebuffer; restore
	 * the LFB's WC/PCD choice if so. */
	paging_lfb_refresh();
	if (!lapic_init_bsp()) {
		serial_puts(SERIAL_COM1, "smp: BSP LAPIC init failed\n");
		return;
	}
	cpus[0].lapic_id = lapic_id();
	if (!copy_trampoline()) {
		serial_puts(SERIAL_COM1, "smp: bad AP trampoline, staying UP\n");
		return;
	}

	for (i = 1; i < n; i++) {
		unsigned int top = process_init_ap(i);

		if (!top) {
			serial_puts(SERIAL_COM1, "smp: no stack for AP\n");
			break;
		}
		start_one_ap(i, top);
		if (!ap_started[i]) {
			serial_puts(SERIAL_COM1, "smp: AP ");
			serial_putdec(SERIAL_COM1, i);
			serial_puts(SERIAL_COM1, " did not start\n");
			break;
		}
		online_aps++;
		cpu_count = online_aps + 1;
	}

	serial_puts(SERIAL_COM1, "smp: ");
	serial_putdec(SERIAL_COM1, cpu_count);
	serial_puts(SERIAL_COM1, " cpu online\n");
}

void smp_ap_main(unsigned int cpu)
{
	struct process *idle;

	if (cpu == 0 || cpu >= MAX_CPUS) {
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	gdt_tss_init(cpu);
	idt_load_ap();
	/* An AP starts with the firmware's CR4; enable the SSE state before it
	 * can reach the drawing fast paths (the BSP does this lazily). */
	cpu_enable_sse();
	cpus[cpu].id = cpu;
	cpus[cpu].lapic_id = lapic_id();
	idle = cpus[cpu].idle;
	if (!idle)
		for (;;)
			__asm__ volatile ("cli; hlt");
	cpus[cpu].current = idle;
	idle->state = PROC_RUNNING;
	idle->on_cpu = cpu;
	idle->last_cpu = cpu;
	cpus[cpu].tss->esp0 = idle->kstack_top;

	lapic_init_ap();
	if (!lapic_timer_start(LAPIC_TIMER_VECTOR))
		for (;;)
			__asm__ volatile ("cli; hlt");

	ap_started[cpu] = 1;
	__sync_synchronize();

	__asm__ volatile ("sti");
	for (;;)
		cpu_idle();
}

void smp_resched_cpu(unsigned int cpu)
{
	if (cpu >= cpu_count || cpu >= MAX_CPUS)
		return;
	if (cpu == this_cpu()->id) {
		cpus[cpu].need_resched = 1;
		return;
	}
	if (cpus[cpu].lapic_id == 0xFFFFFFFFu)
		return;
	lapic_send_ipi(cpus[cpu].lapic_id, LAPIC_RESCHED_VECTOR);
}

void smp_enable_sched(void)
{
	unsigned int i;

	if (cpu_count <= 1)
		return;
	smp_sched_enabled = 1;
	for (i = 1; i < cpu_count; i++)
		smp_resched_cpu(i);
}

void smp_stop_aps(void)
{
	unsigned int i, n;

	/* The kexec trampoline is about to overwrite the kernel.  Stop taking
	 * interrupts first (also on UP, so the final copy cannot be
	 * preempted), park every AP, and wait long enough for INIT to take
	 * effect.  An AP may have been inside a scheduler critical section, so
	 * the lock is reinitialized afterwards; the old run queue is dead
	 * anyway and the new kernel re-creates everything. */
	__asm__ volatile ("cli");
	if (cpu_count <= 1)
		return;
	for (i = 1; i < cpu_count; i++) {
		if (cpus[i].lapic_id != 0xFFFFFFFFu)
			lapic_send_init(cpus[i].lapic_id);
	}
	/* A few million PAUSE iterations is well over the 10 ms the INIT
	 * protocol requires on the Atom/P5 range, and costs nothing on the
	 * few occasions a kexec runs. */
	for (n = 0; n < 4000000u; n++)
		__asm__ volatile ("pause");
	sched_lock_reset();
}

void smp_dump(void)
{
	serial_puts(SERIAL_COM1, "smp: online ");
	serial_putdec(SERIAL_COM1, cpu_count);
	serial_puts(SERIAL_COM1, "\n");
}
