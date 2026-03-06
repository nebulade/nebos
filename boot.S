/* Boot sector: load contiguous kernel sectors 1..N to 0x7E00, then PM. No FAT. */

.code16
.section .text

.globl _start
_start:
	.byte 0xEB, 0x3E, 0x90
	.space 0x3E - 3, 0

	.org 0x3E
kernel_sectors:
	.word 1

	.org 0x40
loader_start:
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %ss
	movw  $0x7c00, %sp
	movb  %dl, boot_drive

	movw  $0x0003, %ax
	int   $0x10

	/* Load kernel: sectors 1..kernel_sectors to 0x7E00. */
	movw  kernel_sectors, %cx
	movw  $1, %si
	movw  $0x07e0, %ax
	movw  %ax, %es
	xorw  %bx, %bx

load_loop:
	cmpw  $0, %cx
	jz    load_done
	pushw %cx
	pushw %si
	/* CHS from LBA in SI */
	movw  %si, %ax
	xorw  %dx, %dx
	movw  $18, %di
	divw  %di
	movb  %dl, %cl
	incb  %cl
	xorw  %dx, %dx
	movw  $2, %di
	divw  %di
	movb  %dl, %dh
	movb  %al, %ch
	movb  boot_drive, %dl
	movb  $0x02, %ah
	movb  $1, %al
	int   $0x13
	popw  %si
	popw  %cx
	jc    halt
	addw  $0x200, %bx
	jnc   1f
	movw  %es, %ax
	addw  $0x1000, %ax
	movw  %ax, %es
1:	incw  %si
	decw  %cx
	jnz   load_loop

load_done:
	movw  $0x7DFC, %bx
	movb  boot_drive, %al
	movb  %al, (%bx)

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

	.align 8
gdt:
	.quad 0
	.word 0xffff, 0x0000
	.byte 0x00, 0x9a, 0xcf, 0x00
	.word 0xffff, 0x0000
	.byte 0x00, 0x92, 0xcf, 0x00
gdt_end:
gdtr:
	.word gdt_end - gdt - 1
	.long gdt

halt:
	cli
1:	hlt
	jmp   1b

.code32
pm_start:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  %ax, %gs
	movw  %ax, %ss
	/* Stack above DMA buffer 0x20000-0x20200 so DMA does not overwrite stack */
	movl  $0x20800, %esp
	movl  $0x7e00, %eax
	jmp   *%eax

.section .bootsig
.byte 0x55, 0xaa
