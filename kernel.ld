/* Kernel runs at 0x7E00; entry section first so 0x7E00 = kernel_main */
ENTRY(kernel_main)

SECTIONS
{
	. = 0x7E00;
	.text : AT(0x7E00) {
		*(.text.entry)
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
ASSERT(_end <= 0x20000, "Kernel too large would overwrite stack/DMA at 0x20000");
