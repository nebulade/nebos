/* Boot sector: one 512-byte section at 0x7C00, signature at 510-511 */
ENTRY(_start)

SECTIONS
{
	. = 0x7C00;
	.boot : AT(0x7C00) {
		*(.text)
		*(.rodata .rodata.*)
		. = 0x7C00 + 510;
		*(.bootsig)
		. = 0x7C00 + 512;
	}
	/DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
