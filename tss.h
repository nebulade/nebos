/* TSS + per-CPU flat GDT.
 *
 * Layout: 0x00 null, 0x08 kernel code, 0x10 kernel data, 0x18 user code,
 * 0x20 user data, 0x28 per-CPU data (GS base), 0x30 + 8*cpu TSS.
 * Every CPU gets its own GDT because the GS descriptor base differs.
 */
#ifndef TSS_H
#define TSS_H

struct tss {
	unsigned int prev_tss;
	unsigned int esp0;
	unsigned int ss0;
	unsigned int esp1, ss1, esp2, ss2;
	unsigned int cr3;
	unsigned int eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
	unsigned int es, cs, ss, ds, fs, gs;
	unsigned int ldt;
	unsigned short trap, iomap_base;
} __attribute__((packed));

/* Build/load the GDT and TSS for one CPU and point GS at cpus[cpu].
 * CPU 0 is initialized from kernel_main before paging; AP bring-up will
 * call the same function with its own index later. */
void gdt_tss_init(unsigned int cpu);

#endif
