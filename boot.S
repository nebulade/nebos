/* Boot sector: load contiguous kernel sectors 1..N to 0x7E00,
   then jump to 0x7E00 in real mode (VESA stage handles the rest). */

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

load_kernel:
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
	movb  boot_drive, %dl
	cmpb  $0x80, %dl
	jb    read_chs
	xorw  %ax, %ax
	movw  %ax, 0x50a
	movw  %ax, 0x50c
	movw  %ax, 0x50e
	movb  $0x10, 0x500
	movb  %al, 0x501
	incw  %ax
	movw  %ax, 0x502
	movw  %bx, 0x504
	movw  %es, 0x506
	movw  %si, 0x508
	movw  $0x500, %si
	movb  $0x42, %ah
	jmp   do_int13
read_chs:
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
do_int13:
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
	/* Jump to VESA stage at 0x7E00 (real mode). */
	ljmp  $0x0000, $0x7E00

boot_drive:
	.byte 0

halt:
	cli
1:	hlt
	jmp   1b

.section .bootsig
.byte 0x55, 0xaa
