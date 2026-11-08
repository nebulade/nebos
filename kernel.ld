/* VESA real-mode stage at 0x7E00, then kernel_main follows. */
ENTRY(_vesa_stage)

SECTIONS
{
	. = 0x7E00;
	.text : AT(0x7E00) {
		*(.text.stage2)
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	.bss : {
		__bss_start = .;
		*(.bss .bss.*) *(COMMON)
		__bss_end = .;
	}
	/* Bootstrap/protected-mode stack: inside the image's RAM footprint
	 * (NOLOAD, so it costs no file bytes), which means its address travels
	 * with the kernel instead of being a fixed one that kernel growth has
	 * to be scheduled around.  It must sit *after* __bss_end: the BSS clear
	 * would otherwise wipe the stack kernel_main is running on. */
	.stack (NOLOAD) : {
		. = ALIGN(16);
		__boot_stack_bottom = .;
		. += 0x2000;             /* 8 KB */
		__boot_stack_top = .;
	}
	_end = .;
}
/* The legacy stage now copies each rootfs chunk straight to PACK_HIGH_ADDR,
 * so the old 0x60000..0x9FBFF low staging gap is no longer used.  BSS and
 * the boot stack may occupy that range.  Keep the kernel end below 0x90000
 * so the top of the low-memory gap and the EBDA stay untouched. */
ASSERT(_end <= 0x90000, "Kernel too large: would reach the top of the low-memory gap");
