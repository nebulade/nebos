/* User program (ring-3): load at 0x40000000, the canonical user VA. */
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
