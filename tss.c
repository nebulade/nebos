/* TSS + flat GDT (kernel code/data, user code/data, TSS). */

#include "tss.h"

/* Top of the in-image bootstrap stack (see kernel.ld): used until the first
 * context switch replaces TSS.esp0 with the process's own kernel stack. */
extern char __boot_stack_top[];

struct tss tss;

struct gdt_entry {
	unsigned short limit_low;
	unsigned short base_low;
	unsigned char  base_mid;
	unsigned char  access;
	unsigned char  limit_high_flags;
	unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
	unsigned short limit;
	unsigned int   base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gdtp;

static void set_entry(int i, unsigned int base, unsigned int limit,
		      unsigned char access, unsigned char flags)
{
	gdt[i].limit_low  = (unsigned short)(limit & 0xFFFF);
	gdt[i].base_low   = (unsigned short)(base & 0xFFFF);
	gdt[i].base_mid   = (unsigned char)((base >> 16) & 0xFF);
	gdt[i].access     = access;
	gdt[i].limit_high_flags =
		(unsigned char)(((limit >> 16) & 0x0F) | (flags & 0xF0));
	gdt[i].base_high  = (unsigned char)((base >> 24) & 0xFF);
}

void gdt_tss_init(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(gdt); i++)
		((unsigned char *)gdt)[i] = 0;
	for (i = 0; i < sizeof(tss); i++)
		((unsigned char *)&tss)[i] = 0;

	/* 0x00 null */
	/* 0x08 kernel code, 0x10 kernel data */
	set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
	set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
	/* 0x18 user code, 0x20 user data (DPL=3) */
	set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
	set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);
	/* 0x28 TSS */
	set_entry(5, (unsigned int)&tss, sizeof(tss) - 1, 0x89, 0x00);

	gdtp.limit = (unsigned short)(sizeof(gdt) - 1);
	gdtp.base  = (unsigned int)gdt;
	__asm__ volatile ("lgdt %0" : : "m"(gdtp));

	tss.ss0  = 0x10;    /* kernel data segment */
	tss.esp0 = (unsigned int)__boot_stack_top;
	tss.iomap_base = 0xFFFF;  /* no I/O permission bitmap (deny ring-3 I/O) */
	__asm__ volatile ("ltr %0" : : "r"((unsigned short)0x28));
}
