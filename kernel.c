/* Kernel: entry point at 0x7E00, timer (PIT 1 kHz), process init, terminal. */

#include "idle.h"
#include "io.h"
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
#include "sys_gfx.h"
#include "timer.h"
#include "tss.h"
#include "usbhid.h"
#include "wm.h"
#include "serial.h"
#include "version.h"
#include "vga.h"
#include "vfs.h"

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

volatile unsigned int ticks;

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
extern void syscall_stub(void);
extern unsigned int exc_stub_table[];

unsigned int get_ticks_ms(void)
{
	return ticks;
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

	/* Syscall gate (ring-3 can int 0x80). */
	set_idt_gate_user(0x80, (unsigned int)syscall_stub);
	/* Kernel-only voluntary reschedule gate (int 0x81). */
	set_idt_gate(0x81, (unsigned int)resched_stub);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(1193 & 0xFF));
	outb(0x40, (unsigned char)((1193 >> 8) & 0xFF));

	__asm__ volatile ("sti");
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
		    !process_is_alive((unsigned int)kworker_pid))
			system_housekeeping();
		cpu_idle();
	}
}

void kernel_main(void);
void kernel_main(void)
{
	unsigned char *p = __bss_start;
	while (p < __bss_end)
		*p++ = 0;

	serial_init(SERIAL_COM1);
	serial_ring_init();
	/* Network console mirror from the very first line on; the ring holds the
	 * early boot output until the WM loop starts flushing it. */
	netlog_init();
	serial_puts(SERIAL_COM1, "\n" OS_NAME " v" OS_VERSION " booted\n");

	/* GDT/TSS and the per-CPU GS base must exist before anything that can
	 * use current_proc()/this_cpu().  The BSS is already cleared and the
	 * GDT/TSS live at identity-mapped addresses. */
	gdt_tss_init(0);

	mm_init();
	mm_dump();
	kmalloc_init();
	paging_init();
	io_init();
	idle_init();

	/* process_init must run before timer_init: once the PIT is started (sti
	 * in timer_init) the context-switch stub runs on every tick and reads
	 * the current process, its CR3 and the TSS, so those must be valid. */
	process_init();
	timer_init();
	mouse_init();
	/* USB HID backend (mouse + keyboard): controller bring-up + first
	 * scan; devices plugged in later are picked up by the hotplug
	 * watcher in the WM loop. */
	usbhid_init();
	vga_shadow_init();
	vga_fb_flush();
	vfs_mount_all();
	/* The command pack: read-only, mounted at /ramfs (see ramfs.c). */
	{
		extern unsigned char _binary_cmds_pack_start[];
		extern unsigned char _binary_cmds_pack_end[];
		unsigned int pack_len = (unsigned int)(_binary_cmds_pack_end -
						       _binary_cmds_pack_start);
		if (ramfs_init(_binary_cmds_pack_start, pack_len) != 0)
			serial_puts(SERIAL_COM1, "ramfs: bad pack\n");
		else {
			serial_puts(SERIAL_COM1, "ramfs: pack ok, ");
			serial_putdec(SERIAL_COM1, ramfs_count());
			serial_puts(SERIAL_COM1, " files\n");
			vfs_mount_ramfs();
		}
	}
	/* Bring the NIC up before the WM loop starts, so an idle board can pick
	 * up a network kexec (`make net-dev`) without any typing. */
	netdev_active();
	/* With a NIC present, try DHCP; on failure the static default stays. */
	if (netdev_active())
		dhcp_run(3000);
	/* The idle process is the kernel_main context; the ring-3 WM is spawned
	 * from /ramfs/bin/wm.  Housekeeping runs in its own process so it cannot
	 * be starved by a busy system. */
	wm_server_init();
	wm_server_start();
	kworker_pid = process_create("kworker", kworker_main);
	if (kworker_pid < 0) {
		serial_puts(SERIAL_COM1, "kworker: cannot create, using idle fallback\n");
		kworker_pid = -1;
	} else {
		process_set_weight((unsigned int)kworker_pid, 1);
		process_set_affinity((unsigned int)kworker_pid, 1);
	}
	kernel_idle_loop();
}
