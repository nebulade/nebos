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
	_end = .;
}
ASSERT(_end <= 0x30000, "Kernel too large would overwrite stack/DMA at 0x30000");
