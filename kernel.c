/* Kernel: entry point at 0x7E00, timer (PIT 1 kHz), then run terminal. */

#include "io.h"
#include "terminal.h"
#include "timer.h"

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
extern unsigned int exc_stub_table[];

void pit_irq(void);

void pit_irq(void)
{
	ticks++;
	outb(0x20, 0x20);
}

unsigned int get_ticks_ms(void)
{
	return ticks;
}

void exc_handler(unsigned int vector);
void exc_handler(unsigned int vector)
{
	static const char * const names[] = {
		"#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint",
		"#OF Overflow", "#BR Bound Range", "#UD Invalid Opcode",
		"#NM No FPU", "#DF Double Fault", "Coprocessor Overrun",
		"#TS Invalid TSS", "#NP Seg Not Present", "#SS Stack Fault",
		"#GP General Protection", "#PF Page Fault", "Reserved",
		"#MF FPU Error", "#AC Alignment Check", "#MC Machine Check",
		"#XM SIMD Error",
	};
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

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(1193 & 0xFF));
	outb(0x40, (unsigned char)((1193 >> 8) & 0xFF));

	__asm__ volatile ("sti");
}

extern unsigned char __bss_start[], __bss_end[];

void kernel_main(void) __attribute__((section(".text.entry")));
void kernel_main(void)
{
	unsigned char *p = __bss_start;
	while (p < __bss_end)
		*p++ = 0;

	io_init();
	timer_init();
	terminal_run();
}
