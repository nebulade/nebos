/* MBR linker script: places code at offset 0 for raw binary output. */
ENTRY(_start)

SECTIONS
{
	. = 0x0;
	.boot : AT(0x0) {
		*(.text)
		*(.rodata .rodata.*)
		. = 0x1FE;
		*(.bootsig)
		. = 0x200;
	}
	/DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
