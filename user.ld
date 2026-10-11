/* User program: load at 0x43000, same as run.c PROGRAM_LOAD_ADDR. */
ENTRY(_start)

SECTIONS
{
	. = 0x43000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	_end = .;
}
