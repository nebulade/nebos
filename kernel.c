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

static void timer_init(void)
{
	unsigned int stub = (unsigned int)pit_irq_stub;

	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x20);
	outb(0xA1, 0x28);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFE);
	outb(0xA1, 0xFF);

	idt[0x20].offset_lo = (unsigned short)(stub & 0xFFFF);
	idt[0x20].selector  = 0x08;
	idt[0x20].zero      = 0;
	idt[0x20].type      = 0x8E;
	idt[0x20].offset_hi = (unsigned short)((stub >> 16) & 0xFFFF);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(1193 & 0xFF));
	outb(0x40, (unsigned char)((1193 >> 8) & 0xFF));

	__asm__ volatile ("sti");
}

void kernel_main(void) __attribute__((section(".text.entry")));
void kernel_main(void)
{
	io_init();
	timer_init();
	terminal_run();
}
