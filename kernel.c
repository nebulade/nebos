/* Freestanding C: print "Welcome to nebos" via BIOS int 0x10 teletype. */

static void bios_putchar(int c)
{
	__asm__ volatile (
		"int $0x10"
		:
		: "a" ((0x0E << 8) | (unsigned char)c),
		  "b" (0x0000)
	);
}

void kernel_main(void)
{
	const char msg[] = "Welcome to nebos";
	unsigned i;

	for (i = 0; msg[i] != '\0'; i++)
		bios_putchar((unsigned char)msg[i]);
}
