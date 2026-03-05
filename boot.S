/* Boot sector stub: run at 0x7C00, set VGA mode 0x03, print message, call kernel_main. */
.code16
.section .text

.globl _start
_start:
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %ss
	movw  $0x7c00, %sp

	/* Set VGA mode 0x03 (80x25 color) via BIOS int 0x10 */
	movw  $0x0003, %ax
	int   $0x10

	/* Print "Welcome to nebos" in green by writing to VGA text buffer at 0xB8000 */
	movw  $0xb800, %ax
	movw  %ax, %es		/* ES = VGA segment */
	xorw  %di, %di		/* start at offset 0 (top-left) */
	movw  $msg, %si
	movb  $0x02, %ah	/* attribute: green on black */
1:	lodsb
	cmpb  $0, %al
	je    2f
	movw  %ax, %es:(%di)	/* write (attr<<8)|char to screen */
	addw  $2, %di
	jmp   1b
2:

	call  kernel_main

	cli
3:	hlt
	jmp   3b

msg:
	.asciz "Welcome to nebos"

.section .bootsig
.byte 0x55, 0xaa
