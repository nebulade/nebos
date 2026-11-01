/* Kernel: entry point at 0x7E00, timer (PIT 1 kHz), process init, terminal. */

#include "acpi.h"
#include "bootcon.h"
#include "cpu.h"
#include "idle.h"
#include "io.h"
#include "lapic.h"
#include "panic.h"
#include "kmalloc.h"
#include "mm.h"
#include "mouse.h"
#include "netdev.h"
#include "dhcp.h"
#include "netlog.h"
#include "paging.h"
#include "process.h"
#include "ramfs.h"
#include "sched.h"
#include "sys_gfx.h"
#include "thermal.h"
#include "timer.h"
#include "tss.h"
#include "usbhid.h"
#include "wm.h"
#include "serial.h"
#include "smp.h"
#include "version.h"
#include "vga.h"
#include "vfs.h"

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

volatile unsigned int ticks;
static unsigned int timer_hz;
static unsigned int tick_ms = 1;

struct idt_entry {
	unsigned short offset_lo;
	unsigned short selector;
	unsigned char  zero;
	unsigned char  type;
	unsigned short offset_hi;
} __attribute__((packed));

struct idtr_desc {
	unsigned short limit;
	unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr_desc idtr;

extern void pit_irq_stub(void);
extern void kbd_irq_stub(void);
extern void fdc_irq_stub(void);
extern void default_irq_stub(void);
extern void resched_stub(void);
extern void lapic_timer_stub(void);
extern void lapic_resched_stub(void);
extern void lapic_spurious_stub(void);
extern void syscall_stub(void);
extern unsigned int exc_stub_table[];

unsigned int get_ticks_ms(void)
{
	return ticks;
}

unsigned int timer_tick_ms(void)
{
	return tick_ms;
}

void timer_tick_advance(void)
{
	ticks += tick_ms;
}

void timer_set_hz(unsigned int hz)
{
	unsigned int flags, divisor;

	if (hz < 100 || hz > 1000)
		hz = 1000;
	if (hz == timer_hz)
		return;
	__asm__ volatile ("pushfl; popl %0" : "=r"(flags));
	__asm__ volatile ("cli" ::: "memory");
	timer_hz = hz;
	tick_ms = 1000 / hz;
	divisor = 1193182u / hz;
	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(divisor & 0xFF));
	outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
	if (flags & (1u << 9))
		__asm__ volatile ("sti" ::: "memory");
}

void exc_handler(struct exc_state *st);
void exc_handler(struct exc_state *st)
{
	unsigned int vector = st->vector;
	unsigned int cr2 = 0;
	static const char * const names[] = {
		"#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint",
		"#OF Overflow", "#BR Bound Range", "#UD Invalid Opcode",
		"#NM No FPU", "#DF Double Fault", "Coprocessor Overrun",
		"#TS Invalid TSS", "#NP Seg Not Present", "#SS Stack Fault",
		"#GP General Protection", "#PF Page Fault", "Reserved",
		"#MF FPU Error", "#AC Alignment Check", "#MC Machine Check",
		"#XM SIMD Error",
	};
	__asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

	/* A fault from ring 3 is a bug in a user command (or its ELF): kill that
	 * process and keep the kernel running.  Kernel faults still halt. */
	if ((st->cs & 3) == 3) {
		serial_puts(SERIAL_COM1, "\n*** user fault vec ");
		serial_puthex(SERIAL_COM1, vector);
		serial_puts(SERIAL_COM1, " eip=");
		serial_puthex(SERIAL_COM1, st->eip);
		serial_puts(SERIAL_COM1, " cr2=");
		serial_puthex(SERIAL_COM1, cr2);
		serial_puts(SERIAL_COM1, " -- process killed\n");
		io_putstr("\n*** fault: ");
		io_putstr(vector < 20 ? names[vector] : "unknown vector");
		io_putstr(" -- command killed\n");
		process_exit();     /* marks us dead and waits to be switched out */
	}
	/* Fatal kernel fault: the banner goes straight into the LFB (no serial
	 * console needed) and the full state is mirrored to COM1. */
	panic_report(st, vector < 20 ? names[vector] : "unknown vector");
	for (;;)
		__asm__ volatile ("cli; hlt");
}

static void set_idt_gate(int vec, unsigned int handler)
{
	idt[vec].offset_lo = (unsigned short)(handler & 0xFFFF);
	idt[vec].selector  = 0x08;
	idt[vec].zero      = 0;
	idt[vec].type      = 0x8E;
	idt[vec].offset_hi = (unsigned short)((handler >> 16) & 0xFFFF);
}

/* Trap gate (DPL=3) so ring-3 can `int` into it; a trap gate does NOT clear
 * IF, so a blocking syscall (e.g. sleep) can still be preempted by the timer. */
static void set_idt_gate_user(int vec, unsigned int handler)
{
	idt[vec].offset_lo = (unsigned short)(handler & 0xFFFF);
	idt[vec].selector  = 0x08;
	idt[vec].zero      = 0;
	idt[vec].type      = 0xEF;
	idt[vec].offset_hi = (unsigned short)((handler >> 16) & 0xFFFF);
}

static void timer_init(void)
{
	int i;

	/* Remap PICs: master at 0x20, slave at 0x28. */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x20);
	outb(0xA1, 0x28);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFC);
	outb(0xA1, 0xFF);

	/* Default handler for all 256 vectors: absorbs any unexpected
	   interrupt (including spurious IRQ 7) with an EOI + iret. */
	for (i = 0; i < 256; i++)
		set_idt_gate(i, (unsigned int)default_irq_stub);

	/* Exception handlers 0-31: ring-3 faults kill the process, kernel
	   faults print the panic banner and halt (see exc_handler). */
	for (i = 0; i < 32; i++)
		set_idt_gate(i, exc_stub_table[i]);

	/* Hardware IRQ handlers. */
	set_idt_gate(0x20, (unsigned int)pit_irq_stub);
	set_idt_gate(0x21, (unsigned int)kbd_irq_stub);
	set_idt_gate(0x26, (unsigned int)fdc_irq_stub);

	/* Local-APIC vectors: the AP timer and reschedule IPI, plus the
	 * spurious vector the LAPIC can deliver at any time. */
	set_idt_gate(LAPIC_TIMER_VECTOR, (unsigned int)lapic_timer_stub);
	set_idt_gate(LAPIC_RESCHED_VECTOR, (unsigned int)lapic_resched_stub);
	set_idt_gate(LAPIC_SPURIOUS_VECTOR, (unsigned int)lapic_spurious_stub);

	/* Syscall gate (ring-3 can int 0x80). */
	set_idt_gate_user(0x80, (unsigned int)syscall_stub);
	/* Kernel-only voluntary reschedule gate (int 0x81). */
	set_idt_gate(0x81, (unsigned int)resched_stub);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	/* Start the PIT at 1 kHz; schedule() drops it to 100 Hz while the BSP
	 * is idle (see timer_set_hz). */
	timer_set_hz(1000);

	__asm__ volatile ("sti");
}

/* The BSP built the IDT once; APs only need to load the same pointer. */
void idt_load_ap(void)
{
	__asm__ volatile ("lidt %0" : : "m"(idtr));
}

extern unsigned char __bss_start[], __bss_end[];

static int kworker_pid = -1;

/* Parts of the system that must keep running independently of the WM:
 * input routing, cursor blink, kexec recovery, netlog flushing, gfx-session
 * cleanup and the shadow-buffer flush. */
static void system_housekeeping(void)
{
	process_reap_dead();
	mouse_poll_update();
	wm_pump_keyboard();
	wm_blink_cursor();
	netlog_flush();
	wm_handle_kexec();
	wm_server_supervise();
	if (wm_server_can_reap())
		sys_gfx_collect();
	if (!wm_server_frame_active())
		vga_fb_flush_budget(VGA_FLUSH_BUDGET_BYTES);
}

static void kworker_main(void)
{
	for (;;) {
		system_housekeeping();
		process_sleep_ms(5);
	}
}

/* Idle is not a normal runnable task: schedule() selects it only when no
 * READY process exists.  The kworker does the housekeeping; if it dies (or
 * could not be created), idle takes over as a degraded fallback. */
static void kernel_idle_loop(void)
{
	for (;;) {
		if (kworker_pid <= 0 ||
		    !process_is_alive((unsigned int)kworker_pid)) {
			/* Idle must never sleep on the BKL: if a user process on
			 * another CPU owns it, just keep idling and retry. */
			if (bkl_trylock()) {
				system_housekeeping();
				bkl_unlock();
			}
		}
		cpu_idle();
	}
}

/* Boot-stage tracing: COM1 (netlog mirrors it) plus the direct-LFB boot
 * console, so a machine without serial still shows where the boot stops. */
static unsigned int boot_t0;

static void boot_trace(const char *what)
{
	unsigned int ms = get_ticks_ms() - boot_t0;

	bootcon_puts("[");
	bootcon_puts(what);
	bootcon_puts("] +");
	bootcon_dec(ms);
	bootcon_puts(" ms\n");

	serial_puts(SERIAL_COM1, "[boot] ");
	serial_puts(SERIAL_COM1, what);
	serial_puts(SERIAL_COM1, " +");
	serial_putdec(SERIAL_COM1, ms);
	serial_puts(SERIAL_COM1, " ms\n");
}

static void boot_val(const char *name, unsigned int v)
{
	bootcon_puts(name);
	bootcon_puts(": ");
	bootcon_dec(v);
	bootcon_puts("\n");

	serial_puts(SERIAL_COM1, name);
	serial_puts(SERIAL_COM1, ": ");
	serial_putdec(SERIAL_COM1, v);
	serial_puts(SERIAL_COM1, "\n");
}

/* Bounded PIT self-test.  If the 8259 does not deliver IRQ0, every later
 * delay/timeout would spin on a frozen tick counter; say so and stop. */
static int pit_ticking(void)
{
	unsigned int t0 = get_ticks_ms();
	unsigned int n;

	for (n = 0; n < 5000000u; n++) {
		if (get_ticks_ms() != t0)
			return 1;
		__asm__ volatile ("pause");
	}
	return 0;
}

void kernel_main(void);
void kernel_main(void)
{
	unsigned char *p = __bss_start;
	while (p < __bss_end)
		*p++ = 0;

	/* Earliest possible output: straight into the boot framebuffer, before
	 * paging, drivers, the heap or the serial port.  Everything below logs
	 * here as well as to COM1, so a machine without serial (the X201) can
	 * show where the boot stops. */
	bootcon_init();
	bootcon_puts(OS_NAME " v" OS_VERSION "\n");

	serial_init(SERIAL_COM1);
	serial_ring_init();
	/* Network console mirror from the very first line on; the ring holds the
	 * early boot output until the WM loop starts flushing it. */
	netlog_init();
	serial_puts(SERIAL_COM1, "\n" OS_NAME " v" OS_VERSION " booted\n");
	boot_trace("start");

	/* GDT/TSS and the per-CPU GS base must exist before anything that can
	 * use current_proc()/this_cpu().  The BSS is already cleared and the
	 * GDT/TSS live at identity-mapped addresses. */
	gdt_tss_init(0);
	boot_trace("gdt");

	mm_init();
	mm_dump();
	boot_trace("mm");
	boot_val("mm free KB", mm_free_pages() * 4);
	kmalloc_init();
	paging_init();
	boot_trace("paging");
	/* paging_init only ORs in PSE; make sure the BSP's SSE state is on
	 * before any drawing path can select an SSE2 helper. */
	cpu_enable_sse();
	/* ACPI needs paging for table access; a missing or unusable MADT simply
	 * leaves the machine at one CPU. */
	acpi_init();
	acpi_dump();
	boot_trace("acpi");
	boot_val("acpi cpus", acpi_cpu_count());
	io_init();
	boot_trace("io");
	idle_init();
	thermal_init();
	boot_trace("idle");

	/* process_init must run before timer_init: once the PIT is started (sti
	 * in timer_init) the context-switch stub runs on every tick and reads
	 * the current process, its CR3 and the TSS, so those must be valid. */
	process_init();
	boot_trace("process");
	timer_init();
	boot_t0 = get_ticks_ms();
	boot_trace("timer");
	if (!pit_ticking()) {
		bootcon_puts("*** PIT NOT TICKING ***\n");
		serial_puts(SERIAL_COM1, "*** PIT NOT TICKING ***\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}
	boot_val("timer ticks", get_ticks_ms());
	/* The PIT (and its sti) is live, so the AP bring-up can use it as a
	 * timeout clock.  APs idle until smp_enable_sched() after boot. */
	smp_init();
	boot_trace("smp");
	boot_val("smp cpus", cpu_count);
	mouse_init();
	boot_trace("ps2");
	/* USB HID backend (mouse + keyboard): controller bring-up + first
	 * scan; devices plugged in later are picked up by the hotplug
	 * watcher in the WM loop. */
	usbhid_init();
	boot_trace("usb");
	vga_shadow_init();
	vga_fb_flush();
	boot_trace("vga");
	bootcon_fb_info();

	/* The command pack: read-only, mounted at /ramfs (see ramfs.c).  This
	 * happens before the block-device scan because the WM and terminal are
	 * loaded from /ramfs, not from a disk. */
	{
		extern unsigned char _binary_cmds_pack_gz_bin_start[];
		extern unsigned char _binary_cmds_pack_gz_bin_end[];
		unsigned int pack_len =
			(unsigned int)(_binary_cmds_pack_gz_bin_end -
				       _binary_cmds_pack_gz_bin_start);
		if (ramfs_init(_binary_cmds_pack_gz_bin_start, pack_len) != 0)
			serial_puts(SERIAL_COM1, "ramfs: bad pack\n");
		else {
			serial_puts(SERIAL_COM1, "ramfs: pack ok, ");
			serial_putdec(SERIAL_COM1, ramfs_count());
			serial_puts(SERIAL_COM1, " files, ");
			serial_putdec(SERIAL_COM1, pack_len);
			serial_puts(SERIAL_COM1, " -> ");
			serial_putdec(SERIAL_COM1, ramfs_pack_size());
			serial_puts(SERIAL_COM1, " bytes\n");
			vfs_mount_ramfs();
		}
	}
	boot_trace("ramfs");
	boot_val("ramfs files", ramfs_count());

	/* Start the WM before touching disks: the first frame can be painted
	 * while storage and the network are still being probed. */
	wm_server_init();
	boot_trace("wm init");
	/* The WM's first frame covers this console; stop drawing before it can
	 * run, or a later stage line would appear on top of the desktop. */
	bootcon_set_enabled(0);
	wm_server_start();
	boot_trace("wm");

	/* Probe the NIC (link negotiation is asynchronous now) before the
	 * kworker starts, so its netlog flushes have a device to use. */
	netdev_active();
	boot_trace("net");

	/* The idle process is the kernel_main context; housekeeping runs in
	 * its own process so it cannot be starved by a busy system. */
	kworker_pid = process_create("kworker", kworker_main);
	if (kworker_pid < 0) {
		serial_puts(SERIAL_COM1, "kworker: cannot create, using idle fallback\n");
		kworker_pid = -1;
	} else {
		process_set_weight((unsigned int)kworker_pid, 1);
		process_set_affinity((unsigned int)kworker_pid, 1);
	}
	boot_trace("kworker");

	/* Slow storage bring-up happens behind the usable desktop now. */
	boot_trace("storage begin");
	vfs_mount_all();
	boot_trace("storage");

	/* The first terminal is spawned only now: while the boot scan was
	 * rebuilding the mount table, no user command could touch the disks.
	 * The desktop itself was already painted from /ramfs. */
	wm_spawn_terminal();
	boot_trace("terminal");

	/* With a NIC present, try DHCP; on failure the static default stays.
	 * Deliberately after the WM: a missing cable or DHCP server must not
	 * delay the first frame. */
	if (netdev_active())
		dhcp_run(3000);
	boot_trace("dhcp");

	/* Boot-time device/FS setup is complete; APs may now take user work.
	 * Kernel threads stay pinned to the BSP and serialize on the BKL. */
	smp_enable_sched();
	boot_trace("sched");
	kernel_idle_loop();
}
