/* Boot sector: load at 0x7C00, exactly 512 bytes, signature at 510-511. */
ENTRY(_start)

SECTIONS
{
	. = 0x7C00;
	.boot : AT(0x7C00) {
		*(.text)
		*(.rodata*)
		. = 0x7C00 + 0x1FE;
		*(.bootsig)
		. = 0x7C00 + 0x200;
	}
	/DISCARD/ : { *(.eh_frame) *(.note.GNU-stack) *(.note.gnu.property) }
}
