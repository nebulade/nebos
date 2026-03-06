/* Reboot command: trigger a CPU reset via the keyboard controller. */

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

void program_reboot_main(void)
{
	/* Pulse the CPU reset line via the keyboard controller (port 0x64). */
	outb(0x64, 0xFE);
	for (;;)
		__asm__ volatile ("hlt");
}
