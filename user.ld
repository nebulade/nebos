/* User program: load at 0x80000, same as run.c PROGRAM_LOAD_ADDR. */
ENTRY(_start)

SECTIONS
{
	. = 0x80000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	_end = .;
}
