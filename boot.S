/* Minimal x86 boot sector: load kernel from sector 2 to 0x7E00,
 * switch to protected mode, jump to kernel (C code at 0x7E00). */

.code16
.section .text

.globl _start
_start:
	/* Segment and stack for real mode */
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %ss
	movw  $0x7c00, %sp

	/* Save boot drive (BIOS puts it in DL) */
	movb  %dl, boot_drive

	/* Set VGA mode 0x03 (80x25 color text) so 0xB8000 is the visible framebuffer */
	movw  $0x0003, %ax
	int   $0x10

	/* Load kernel: 4 sectors from sector 2 into 0x7E00 (ES:BX = 0x07E0:0) */
	movw  $0x07e0, %ax
	movw  %ax, %es
	xorw  %bx, %bx
	movb  $0x02, %ah          /* int 0x13 read */
	movb  $4, %al             /* 4 sectors */
	xorw  %cx, %cx
	movb  $2, %cl             /* sector 2 (1-based) */
	xorw  %dx, %dx             /* head 0, drive in DL */
	movb  boot_drive, %dl
	int   $0x13
	jc    halt                /* on error just halt */

	/* Protected mode: load GDT, set CR0.PE, far jump to 32-bit code */
	cli
	lgdt  gdtr
	movl  %cr0, %eax
	orl   $1, %eax
	movl  %eax, %cr0
	.byte 0x66, 0xea
	.long pm_start
	.word 0x08

	.align 4
boot_drive:
	.byte 0

/* GDT (flat 4GB code and data) */
	.align 8
gdt:
	.quad 0x0000000000000000
	/* 0x08: 32-bit code, DPL=0, base=0, limit=0xfffff (4GB) */
	.word 0xffff, 0x0000
	.byte 0x00, 0x9a, 0xcf, 0x00
	/* 0x10: 32-bit data */
	.word 0xffff, 0x0000
	.byte 0x00, 0x92, 0xcf, 0x00
gdt_end:

gdtr:
	.word gdt_end - gdt - 1
	.long gdt                 /* linear address (linker uses 0x7C00 base) */

halt:
	cli
1:	hlt
	jmp   1b

/* --- 32-bit protected mode --- */
.code32
pm_start:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	movl  $0x8000, %esp       /* stack at 0x8000 */

	/* Jump to kernel at 0x7E00 (direct; *0x7e00 would be indirect = wrong) */
	movl  $0x7e00, %eax
	jmp   *%eax

1:	cli
	hlt
	jmp   1b

/* Boot signature at offset 510 (linker places it) */
.section .bootsig
.byte 0x55, 0xaa
