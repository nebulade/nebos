/*
 * MBR boot code: finds active partition, loads its boot sector, jumps to it.
 * Placed at sector 0 of usb.img. Partition table points to boot sector at sector 1.
 */

.code16
.section .text

.globl _start
_start:
	/* Standard MBR jump */
	.byte 0xEB, 0x44, 0x90
	.space 0x44 - 3, 0

	.org 0x44
mbr_start:
	cli
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %ss
	movw  $0x7c00, %sp
	sti

	/* Save boot drive */
	movb  %dl, boot_drive

	/* Find active partition */
	movw  $0x1BE, %si
	movb  $4, %cl

find_active:
	cmpb  $0x80, (%si)
	je    found_active
	addw  $16, %si
	decb  %cl
	jnz   find_active

	/* No active partition found */
	movw  $no_boot_msg, %si
	call  print_string
	jmp   halt

found_active:
	/* Load boot sector from partition start LBA */
	movl  8(%si), %eax        /* LBA start of partition */
	movl  %eax, part_lba

	/* Read boot sector to 0x7C00 */
	pushw %ds
	popw  %es
	movw  $0x7C00, %bx

read_boot_sector:
	movl  part_lba, %eax
	call  read_sector_lba
	jc    read_error

	/* Check boot signature */
	movw  $0x7DFE, %si
	cmpw  $0xAA55, (%si)
	jne   read_error

	/* Jump to boot sector */
	movb  boot_drive, %dl
	ljmp  $0x0000, $0x7C00

read_error:
	movw  $read_err_msg, %si
	call  print_string
	jmp   halt

/* Read one sector from LBA using INT 0x13 AH=0x42 */
read_sector_lba:
	/* %eax = LBA, %es:%bx = buffer */
	pushw %ds
	movw  $0x500, %si
	movw  %si, %ds
	xorw  %di, %di
	movb  $0x10, (%di)        /* packet size */
	movb  $1, 1(%di)          /* count = 1 */
	movw  %bx, 4(%di)         /* buffer offset */
	movw  %es, 6(%di)         /* buffer segment */
	movl  %eax, 8(%di)        /* LBA */
	xorl  12(%di), %eax       /* LBA high = 0 */
	popw  %ds

	movb  $0x42, %ah
	movb  boot_drive, %dl
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

/* Partition table: one active partition at LBA 1 */
	.byte 0x80              /* bootable */
	.byte 0x00, 0x01, 0x00  /* CHS start */
	.byte 0x06              /* type: FAT16 LBA */
	.byte 0xFF, 0xFF, 0xFF  /* CHS end */
	.long 1                 /* LBA start (boot sector) */
	.long 131071            /* sector count */
	.space 48, 0            /* 3 empty entries */

	/* Boot signature */
.section .bootsig
	.word 0xAA55
