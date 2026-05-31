#include "serial.h"
#include "pci.h"

void serial_init(unsigned int port)
{
	port_outb(port + 1, 0x00);
	port_outb(port + 3, 0x80);
	port_outb(port + 0, 0x0C);
	port_outb(port + 1, 0x00);
	port_outb(port + 3, 0x03);
	port_outb(port + 2, 0xC7);
	port_outb(port + 4, 0x03);
}

void serial_putc(unsigned int port, char c)
{
	while (!(port_inb(port + 5) & 0x20))
		;
	port_outb(port, (unsigned char)c);
}

void serial_puts(unsigned int port, const char *s)
{
	while (*s) {
		if (*s == '\n')
			serial_putc(port, '\r');
		serial_putc(port, *s++);
	}
}

void serial_puthex(unsigned int port, unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;
	serial_puts(port, "0x");
	for (i = 28; i >= 0; i -= 4)
		serial_putc(port, hex[(n >> i) & 0xF]);
}

void serial_putdec(unsigned int port, unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;
	*p = '\0';
	if (n == 0) {
		serial_putc(port, '0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	serial_puts(port, p);
}

int serial_has_data(unsigned int port)
{
	return (port_inb(port + 5) & 0x01) != 0;
}

char serial_getc_nb(unsigned int port)
{
	if (!serial_has_data(port))
		return 0;
	return (char)port_inb(port);
}
