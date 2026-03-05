/* Kernel: entered from bootloader in 32-bit protected mode at 0x7E00.
 * Must be linked so this function is at 0x7E00 (see kernel.ld). */

#define VGA_BASE ((volatile unsigned short *)0xB8000)
#define VGA_ATTR 0x0A  /* green on black */

/* String in .text so it loads with the kernel */
static const char msg[] __attribute__((section(".text"))) = "Hello from C (protected mode)";

/* Entry point: must be first in .text so bootloader jump to 0x7E00 hits it */
void kernel_main(void) __attribute__((section(".text.entry")));
void kernel_main(void)
{
	unsigned i;

	/* Clear screen */
	for (i = 0; i < 80 * 25; i++)
		VGA_BASE[i] = (unsigned short)(VGA_ATTR << 8);

	/* Print message */
	for (i = 0; msg[i] != '\0'; i++)
		VGA_BASE[i] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)msg[i]);

	for (;;)
		__asm__ volatile ("hlt");
}
