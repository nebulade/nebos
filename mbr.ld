/* MBR: one 512-byte section at 0x7C00. mbr.S pads to offset 510 and emits
 * the 0xAA55 signature inline, so objcopy -O binary yields exactly 512 bytes. */
ENTRY(_start)

SECTIONS
{
	. = 0x7C00;
	.boot : {
		*(.text)
		*(.rodata .rodata.*)
	}
	/DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
