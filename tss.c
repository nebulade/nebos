/* TSS + per-CPU flat GDT (see tss.h for the selector layout). */

#include "tss.h"
#include "sched.h"

/* Top of the in-image bootstrap stack (see kernel.ld): used until the first
 * context switch replaces TSS.esp0 with the process's own kernel stack. */
extern char __boot_stack_top[];

#define GDT_ENTRIES (6 + MAX_CPUS)

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

static struct tss tss[MAX_CPUS];
static struct gdt_entry gdt[MAX_CPUS][GDT_ENTRIES]
	__attribute__((aligned(8)));

static void set_entry(struct gdt_entry *g, int i, unsigned int base,
		      unsigned int limit, unsigned char access,
		      unsigned char flags)
{
	g[i].limit_low  = (unsigned short)(limit & 0xFFFF);
	g[i].base_low   = (unsigned short)(base & 0xFFFF);
	g[i].base_mid   = (unsigned char)((base >> 16) & 0xFF);
	g[i].access     = access;
	g[i].limit_high_flags =
		(unsigned char)(((limit >> 16) & 0x0F) | (flags & 0xF0));
	g[i].base_high  = (unsigned char)((base >> 24) & 0xFF);
}

void gdt_tss_init(unsigned int cpu)
{
	struct cpu *c;
	struct gdt_entry *g;
	struct gdt_ptr gp;
	unsigned short sel;
	unsigned int i;

	if (cpu >= MAX_CPUS)
		return;

	c = &cpus[cpu];
	g = gdt[cpu];

	for (i = 0; i < sizeof(gdt[cpu]); i++)
		((unsigned char *)g)[i] = 0;
	for (i = 0; i < sizeof(tss[cpu]); i++)
		((unsigned char *)&tss[cpu])[i] = 0;

	c->self = c;
	c->tss = &tss[cpu];
	c->id = cpu;

	/* 0x00 null */
	/* 0x08 kernel code, 0x10 kernel data */
	set_entry(g, 1, 0, 0xFFFFF, 0x9A, 0xCF);
	set_entry(g, 2, 0, 0xFFFFF, 0x92, 0xCF);
	/* 0x18 user code, 0x20 user data (DPL=3) */
	set_entry(g, 3, 0, 0xFFFFF, 0xFA, 0xCF);
	set_entry(g, 4, 0, 0xFFFFF, 0xF2, 0xCF);
	/* 0x28 per-CPU data: GS base = this CPU's state */
	set_entry(g, 5, (unsigned int)c, 0xFFFFF, 0x92, 0xCF);
	/* 0x30 + 8*cpu: one 32-bit TSS per CPU */
	for (i = 0; i < MAX_CPUS; i++)
		set_entry(g, 6 + i, (unsigned int)&tss[i],
			  sizeof(tss[i]) - 1, 0x89, 0x00);

	tss[cpu].ss0 = 0x10;    /* kernel data segment */
	tss[cpu].esp0 = (unsigned int)__boot_stack_top;
	tss[cpu].iomap_base = 0xFFFF;  /* no I/O permission bitmap */

	gp.limit = (unsigned short)(sizeof(gdt[cpu]) - 1);
	gp.base  = (unsigned int)&g[0];
	__asm__ volatile ("lgdt %0" : : "m"(gp));

	sel = (unsigned short)(0x30 + 8 * cpu);
	__asm__ volatile ("ltr %0" : : "r"(sel));

	sel = 0x28;
	__asm__ volatile ("movw %0, %%gs" : : "r"(sel));

	cpu_gs_ready = 1;
}
