/* Handoff trampoline at 0x800 (the same low-memory slot kexec uses). */
ENTRY(_start)

SECTIONS
{
	. = 0x800;
	.text : {
		*(.text)
		*(.data .data.*)
		*(.rodata .rodata.*)
	}
	/DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
