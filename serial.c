/* Serial port (COM1 0x3F8) debug output for bare-metal diagnostics. */

#include "serial.h"
#include "pci.h"

#define COM1 0x3F8

void serial_init(void)
{
	port_outb(COM1 + 1, 0x00);   /* disable interrupts */
	port_outb(COM1 + 3, 0x80);   /* DLAB on */
	port_outb(COM1 + 0, 0x0C);   /* divisor 12 = 9600 baud */
	port_outb(COM1 + 1, 0x00);
	port_outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
	port_outb(COM1 + 2, 0xC7);   /* enable FIFO */
	port_outb(COM1 + 4, 0x03);   /* RTS + DTR */
}

void serial_putc(char c)
{
	while (!(port_inb(COM1 + 5) & 0x20))
		;
	port_outb(COM1, (unsigned char)c);
}

void serial_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			serial_putc('\r');
		serial_putc(*s++);
	}
}

void serial_puthex(unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;
	serial_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		serial_putc(hex[(n >> i) & 0xF]);
}

void serial_putdec(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;
	*p = '\0';
	if (n == 0) {
		serial_putc('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	serial_puts(p);
}
