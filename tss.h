/* TSS + flat GDT (kernel/user segments + TSS descriptor). */
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

extern struct tss tss;

/* Build the 6-entry GDT (null / kernel code / kernel data / user code / user
 * data / TSS), load it, and load the TSS.  Call once from kernel_main. */
void gdt_tss_init(void);

#endif
