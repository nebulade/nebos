/* AP startup blob, linked at the physical SIPI vector page 0x6000. */
ENTRY(ap_tramp_start)
SECTIONS
{
	. = 0x6000;
	.text : {
		*(.text.ap)
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	ap_tramp_end = .;
}
ASSERT(ap_tramp_end <= 0x6C00,
       "AP trampoline grew into its stack area")
