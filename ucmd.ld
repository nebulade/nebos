/* Ring-3 command process: load at 0x40000000, same as process.h USER_BASE. */
ENTRY(_start)

SECTIONS
{
	. = 0x40000000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	_end = .;
}
