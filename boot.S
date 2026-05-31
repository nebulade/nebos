/* Boot sector: load contiguous kernel sectors 1..N to 0x7E00,
   then jump to 0x7E00 in real mode (VESA stage handles the rest). */

.code16
.section .text

.globl _start
_start:
	.byte 0xEB, 0x3E, 0x90
	/* ---- BPB (BIOS Parameter Block) ---- */
	.ascii "NEBOS   "        /* OEM name */
	.word 512                /* bytes per sector */
	.byte 4                  /* sectors per cluster */
	.word 1                  /* reserved sectors */
	.byte 2                  /* number of FATs */
	.word 512                /* root entries */
	.word 0                  /* total sectors 16-bit (use 32-bit) */
	.byte 0xF8               /* media descriptor (hard disk) */
	.word 128                /* sectors per FAT */
	.word 63                 /* sectors per track */
	.word 128                /* number of heads */
	.long 0                  /* hidden sectors */
	.long 131072             /* total sectors 32-bit */
	/* ---- Extended BPB ---- */
	.byte 0x80               /* drive number */
	.byte 0x00               /* reserved */
	.byte 0x29               /* boot signature */
	.long 0x12345678         /* volume ID */
	.ascii "NEBOS      "     /* volume label */
	.ascii "FAT16   "        /* filesystem type */

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

	.org 0x1BE
/* MBR partition table: one active partition at LBA 0 (USB/HDD). */
	.byte 0x80              /* bootable */
	.byte 0x00, 0x01, 0x00  /* CHS start: C=0, H=0, S=1 */
	.byte 0x06              /* type: FAT16 LBA (matches BPB) */
	.byte 0xFF, 0xFF, 0xFF  /* CHS end: max values */
	.long 0                 /* LBA start */
	.long 131072            /* sector count (64MB) */
	.space 48, 0            /* 3 empty partition entries */

	.org 0x1FE
	.word 0xAA55
