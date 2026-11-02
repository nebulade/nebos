/*
 * MBR boot code: finds active partition, loads its boot sector, jumps to it.
 * Placed at sector 0 of usb.img. Partition table points to boot sector at sector 63.
 *
 * The boot sector is loaded to 0x7C00, i.e. over this code, so the MBR first
 * relocates its whole sector to 0x0600 and continues from there. Data
 * references (scratch, partition table, messages) are relative to the
 * relocated copy via reloc_shift.
 */

.code16
.section .text

	.equ reloc_base,  0x0600
	.set reloc_shift, reloc_base - 0x7C00

.globl _start
_start:
	/* Standard MBR jump to mbr_start at offset 0x44 */
	.byte 0xEB, 0x42, 0x90
	.space 0x44 - 3, 0

	.org 0x44
mbr_start:
	cli
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %ss
	movw  $0x7c00, %sp
	sti

	/* Relocate this sector to 0x0600 and continue there. */
	movw  $0x7c00, %si
	movw  $reloc_base, %di
	movw  $256, %cx
	cld
	rep movsw
	ljmp  $0x0000, $relocated+reloc_shift

relocated:
	/* Save boot drive */
	movb  %dl, boot_drive+reloc_shift

	/* Find active partition */
	movw  $part_table+reloc_shift, %si
	movb  $4, %cl

find_active:
	cmpb  $0x80, (%si)
	je    found_active
	addw  $16, %si
	decb  %cl
	jnz   find_active

	/* No active partition found */
	movw  $no_boot_msg+reloc_shift, %si
	call  print_string
	jmp   halt

found_active:
	/* Load boot sector from partition start LBA */
	movl  8(%si), %eax        /* LBA start of partition */
	movl  %eax, part_lba+reloc_shift

	/* Read boot sector to 0x7C00 */
	pushw %ds
	popw  %es
	movw  $0x7C00, %bx

read_boot_sector:
	movl  part_lba+reloc_shift, %eax
	call  read_sector_lba
	jc    read_error

	/* Check boot signature */
	movw  $0x7DFE, %si
	cmpw  $0xAA55, (%si)
	jne   read_error

	/* Jump to boot sector */
	movb  boot_drive+reloc_shift, %dl
	ljmp  $0x0000, $0x7C00

read_error:
	movw  $read_err_msg+reloc_shift, %si
	call  print_string
	jmp   halt

/* Read one sector from LBA using INT 0x13 AH=0x42 (DS = 0) */
read_sector_lba:
	/* %eax = LBA, %es:%bx = buffer */
	movw  $0x500, %di
	movb  $0x10, (%di)        /* packet size */
	movb  $0, 1(%di)          /* reserved */
	movw  $1, 2(%di)          /* count = 1 */
	movw  %bx, 4(%di)         /* buffer offset */
	movw  %es, 6(%di)         /* buffer segment */
	movl  %eax, 8(%di)        /* LBA */
	movl  $0, 12(%di)         /* LBA high = 0 */

	movw  $0x500, %si         /* DS:SI -> disk address packet */
	movb  $0x42, %ah
	movb  boot_drive+reloc_shift, %dl
	int   $0x13
	ret

/* Print null-terminated string at %si */
print_string:
	pushw %ax
	pushw %bx
	movb  $0x0E, %ah
	movb  $0x00, %bh
1:	lodsb
	testb %al, %al
	jz    2f
	int   $0x10
	jmp   1b
2:	popw  %bx
	popw  %ax
	ret

halt:
	cli
1:	hlt
	jmp   1b

boot_drive:
	.byte 0
part_lba:
	.long 0

no_boot_msg:
	.asciz "No bootable partition"
read_err_msg:
	.asciz "Boot sector read failed"

	/* Pad to partition table at offset 0x1BE (446) */
	.space 0x1BE - (. - _start), 0

part_table:
	/* Partition 1: active, starts at LBA 63 (CHS 0/1/1, the classic DOS
	 * start) and holds the boot sector (LBA 63) and kernel (LBA 64+).
	 * The CHS values match the LBA values for the usual 255 heads /
	 * 63 sectors geometry; BIOSes are known to validate that. */
	.byte 0x80              /* bootable */
	.byte 0x01, 0x01, 0x00  /* CHS start: C=0 H=1 S=1 (LBA 63) */
	.byte 0x06              /* type: FAT16 */
	.byte 0x08, 0x1C, 0x00  /* CHS end: C=0 H=8 S=28 (LBA 531) */
	.long 63                /* LBA start (boot sector) */
	.long 469               /* boot sector + kernel sectors */

	/* Partition 2: dummy in the bare MBR.  usb.img replaces this entry
	 * with the rootfs partition (same start, LBA 532, immediately behind
	 * the kernel); keeping a second valid entry makes the stick look like
	 * a USB hard disk to old AMI BIOSes (e.g. the ASUS AT3IONT-I), which
	 * classify a single-partition MBR as a USB-ZIP disk and never offer
	 * it.  Only partition 1 is active. */
	.byte 0x00              /* not bootable */
	.byte 0x08, 0x1d, 0x00  /* CHS start: C=0 H=8 S=29 (LBA 532) */
	.byte 0x21              /* type: dummy */
	.byte 0x0d, 0x20, 0x01  /* CHS end: C=1 H=13 S=32 (LBA 16915) */
	.long 532
	.long 16384
	.space 32, 0            /* 2 empty entries */

	/* Boot signature at offset 510 */
	.space 510 - (. - _start), 0
	.word 0xAA55
