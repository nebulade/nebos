/* Boot sector: load contiguous kernel sectors 1..N to 0x7E00,
   then jump to 0x7E00 in real mode (VESA stage handles the rest). */

.code16
.section .text

.globl _start
_start:
	.byte 0xEB, 0x40, 0x90
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

	/* First kernel sector (LBA).  Fixed header slot so the image builders
	 * can patch it without depending on the code layout: the floppy image
	 * keeps 1, the USB/HDD image patches the low byte (0x40 = LBA 64). */
	.org 0x40
kernel_lba:
	.word 1

	/* Rootfs image LBA (see rootfs.h).  Patched by the image builders:
	 * floppy/final.bin point it just past the padded kernel, usb.img at
	 * its rootfs partition. */
	.org 0x42
pack_lba:
	.long 1

	.org 0x46
loader_start:
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %ss
	movw  $0x7c00, %sp
	movb  %dl, boot_drive
	cld

	movw  $0x0003, %ax
	int   $0x10

	/* First visible sign of life.  This is before VBE, paging and the
	 * kernel, and it is the only output a machine without serial has:
	 * if the panel shows this line, the BIOS reached our boot sector. */
	movw  $boot_msg, %si
	call  print_string

	/* The kernel load can take a while on a BIOS USB stack; say what is
	 * happening instead of leaving the operator with a blank screen. */
	movw  $loading_msg, %si
	call  print_string

load_kernel:
	movw  kernel_sectors, %cx
	movw  kernel_lba, %si
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
	jc    read_error
	addw  $0x200, %bx
	jnc   1f
	movw  %es, %ax
	addw  $0x1000, %ax
	movw  %ax, %es
1:	incw  %si
	decw  %cx
	jnz   load_loop

load_done:
	/* Second marker: the whole kernel image was read without an INT 13h
	 * error.  It stays on screen if the VESA stage fails or hangs before
	 * it switches modes. */
	movw  $loaded_msg, %si
	call  print_string
	/* Jump to VESA stage at 0x7E00 (real mode). */
	ljmp  $0x0000, $0x7E00

boot_drive:
	.byte 0

/* INT 13h error: say so instead of halting silently. */
read_error:
	movw  $disk_err_msg, %si
	call  print_string
halt:
	cli
1:	hlt
	jmp   1b

/* Print the NUL-terminated string at DS:SI with the BIOS teletype service. */
print_string:
	pushw %ax
	pushw %bx
	movb  $0x0E, %ah
	xorw  %bx, %bx
1:	lodsb
	testb %al, %al
	jz    2f
	int   $0x10
	jmp   1b
2:	popw  %bx
	popw  %ax
	ret

boot_msg:
	.asciz "Nebos starting...\r\n"
loading_msg:
	.asciz "Loading kernel...\r\n"
loaded_msg:
	.asciz "Kernel loaded, starting...\r\n"
disk_err_msg:
	.asciz "Disk read error\r\n"

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
