/* Bootstrap installer: pinned at the OLD build's PROGRAM_LOAD_ADDR (0x4A000).
 *
 * bootstrap.bin is received by a still-running *old* kernel whose `serial recv`
 * writes to its own fixed program address (0x4A000, pre-Stage-A builds), so it
 * must be linked there to run.  New builds never need it: the serial/net kexec
 * auto-detect reloads them.  See bootstrap.c and the AGENTS.md bootstrap flow. */
ENTRY(_start)

SECTIONS
{
	. = 0x4A000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	_end = .;
}
