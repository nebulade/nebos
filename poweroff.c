/* Poweroff command: shut down the virtual machine (QEMU/Bochs ACPI port). */

static inline void outw(unsigned short port, unsigned short v)
{
	__asm__ volatile ("outw %0, %%dx" : : "a"(v), "d"(port));
}

void program_poweroff_main(void)
{
	/* Bochs/QEMU "ACPI" shutdown — see https://wiki.osdev.org/Shutdown */
	outw(0x604, 0x2000);
	outw(0xB004, 0x2000);
	for (;;)
		__asm__ volatile ("hlt");
}
