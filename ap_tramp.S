/*
 * AP startup trampoline, linked at 0x6000 and copied there by the BSP.
 *
 * The BSP fills the fixed info block at 0x6F00 before sending SIPI:
 *   0x6F00 cr3        (physical kernel page directory)
 *   0x6F04 stack_top  (AP idle stack top, identity mapped)
 *   0x6F08 entry      (C entry point, protected-mode address)
 *   0x6F0C cpu_index
 *
 * The AP starts in real mode at 0x6000 (SIPI vector 0x06), builds a tiny flat
 * GDT, enters protected mode, enables PSE + paging and jumps to the C entry.
 */
.code16
.section .text.ap, "ax"

.equ AP_INFO,      0x6F00
.equ AP_INFO_CR3,  AP_INFO + 0
.equ AP_INFO_STACK, AP_INFO + 4
.equ AP_INFO_ENTRY, AP_INFO + 8
.equ AP_INFO_CPU,  AP_INFO + 12

.globl ap_tramp_start
ap_tramp_start:
	cli
	cld
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	movw  $0x6C00, %sp

	lgdt  ap_gdt_desc

	movl  %cr0, %eax
	orl   $1, %eax
	movl  %eax, %cr0

	/* 32-bit far jump into the protected-mode part. */
	.byte 0x66, 0xea
	.long ap_pm32
	.word 0x08

.code32
ap_pm32:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	movl  AP_INFO_STACK, %esp

	movl  AP_INFO_CR3, %eax
	movl  %eax, %cr3

	/* Clear stale translation controls left by the firmware (PAE/PGE/
	 * LA57/PCIDE/SMEP/SMAP) and enable PSE.  An AP that starts with
	 * CR4.PAE set would otherwise walk the kernel page directory as a PAE
	 * PDPT the moment CR0.PG is set. */
	movl  %cr4, %eax
	andl  $0xFFCDEF5F, %eax
	orl   $0x10, %eax             /* CR4.PSE */
	movl  %eax, %cr4

	/* Clear EFER.LME too when long mode exists and the firmware left it
	 * set; otherwise setting CR0.PG would enter long mode. */
	movl  $0x80000000, %eax
	cpuid
	cmpl  $0x80000001, %eax
	jb    3f
	movl  $0x80000001, %eax
	cpuid
	testl $(1 << 29), %edx
	jz    3f
	movl  $0xC0000080, %ecx
	rdmsr
	testl $(1 << 8), %eax
	jz    3f
	andl  $~0x100, %eax
	wrmsr
3:
	movl  %cr0, %eax
	orl   $0x80010000, %eax       /* CR0.PG | CR0.WP */
	movl  %eax, %cr0
	jmp   1f
1:
	movl  AP_INFO_CPU, %eax
	pushl %eax
	movl  AP_INFO_ENTRY, %eax
	call  *%eax

2:	cli
	hlt
	jmp   2b

.align 8
ap_gdt:
	.quad 0
	.word 0xFFFF, 0x0000
	.byte 0x00, 0x9A, 0xCF, 0x00
	.word 0xFFFF, 0x0000
	.byte 0x00, 0x92, 0xCF, 0x00
ap_gdt_end:
ap_gdt_desc:
	.word ap_gdt_end - ap_gdt - 1
	.long ap_gdt
