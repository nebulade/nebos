/* Kernel: entry point at 0x7E00, timer (PIT 1 kHz), process init, terminal. */

#include "idle.h"
#include "io.h"
#include "kdbg.h"
#include "kmalloc.h"
#include "mm.h"
#include "mouse.h"
#include "netdev.h"
#include "netlog.h"
#include "paging.h"
#include "process.h"
#include "timer.h"
#include "tss.h"
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
extern void syscall_stub(void);
extern unsigned int exc_stub_table[];

unsigned int get_ticks_ms(void)
{
	return ticks;
}

void exc_handler(unsigned int eip, unsigned int vector);
void exc_handler(unsigned int eip, unsigned int vector)
{
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
	/* Draw the fault info directly into the framebuffer FIRST: with no
	 * serial console attached, a fault was previously invisible (the text
	 * went to the current process's screen buffer, which the WM never got
	 * to render because the CPU halts here). */
	kdbg_panic(vector < 20 ? names[vector] : "unknown vector", eip, cr2);
	/* Print the fault to serial, using only minimal/no-stack code, so it
	 * survives even if the screen buffer or kernel stack is the problem. */
	serial_puts(SERIAL_COM1, "\n*** EXC vec ");
	serial_puthex(SERIAL_COM1, vector);
	serial_puts(SERIAL_COM1, " eip=");
	serial_puthex(SERIAL_COM1, eip);
	serial_puts(SERIAL_COM1, " cr2=");
	serial_puthex(SERIAL_COM1, cr2);
	serial_puts(SERIAL_COM1, " ");
	if (vector < 20)
		serial_puts(SERIAL_COM1, names[vector]);
	serial_puts(SERIAL_COM1, " ***\n");

	/* Then attempt the screen output (may fault if the screen is broken). */
	io_putstr("\n*** EXCEPTION: ");
	if (vector < 20)
		io_putstr(names[vector]);
	else {
		io_putstr("vector ");
		if (vector >= 10)
			io_putchar((char)('0' + vector / 10));
		io_putchar((char)('0' + vector % 10));
	}
	io_putstr(" ***\nSystem halted.\n");
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

	/* Exception handlers 0-31: print the fault name and halt. */
	for (i = 0; i < 32; i++)
		set_idt_gate(i, exc_stub_table[i]);

	/* Hardware IRQ handlers. */
	set_idt_gate(0x20, (unsigned int)pit_irq_stub);
	set_idt_gate(0x21, (unsigned int)kbd_irq_stub);
	set_idt_gate(0x26, (unsigned int)fdc_irq_stub);

	/* Syscall gate (ring-3 can int 0x80). */
	set_idt_gate_user(0x80, (unsigned int)syscall_stub);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(1193 & 0xFF));
	outb(0x40, (unsigned char)((1193 >> 8) & 0xFF));

	__asm__ volatile ("sti");
}

extern unsigned char __bss_start[], __bss_end[];

void kernel_main(void);
void kernel_main(void)
{
	unsigned char *p = __bss_start;
	while (p < __bss_end)
		*p++ = 0;

	serial_init(SERIAL_COM1);
	serial_ring_init();
	serial_puts(SERIAL_COM1, "\n" OS_NAME " v" OS_VERSION " booted\n");
	mm_init();
	mm_dump();
	kmalloc_init();
	paging_init();
	io_init();
	idle_init();

	/* process_init + gdt_tss_init must run BEFORE timer_init: once the PIT is
	 * started (sti in timer_init) the context-switch stub runs on every tick
	 * and reads procs[].cr3 / procs[].kstack_top / the TSS, so those have to be
	 * valid first (otherwise the first switch loads a garbage CR3 and faults). */
	process_init();
	gdt_tss_init();
	timer_init();
	mouse_init();
	vga_shadow_init();
	vga_fb_flush();
	kdbg_init();
	kdbg_status();
	vfs_mount_all();
	/* Bring the NIC up before the WM loop starts, so an idle board can pick
	 * up a network kexec (`make net-dev`) without any typing. */
	netdev_active();
	/* Network console mirror from the very first line on; the ring holds the
	 * early boot output until the WM loop starts flushing it. */
	netlog_init();
	wm_run();
}
