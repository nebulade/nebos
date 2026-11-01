#include "serial.h"
#include "serial_ring_buf.h"
#include "netlog.h"
#include "pci.h"
#include "sched.h"
#include "timer.h"

void serial_init(unsigned int port)
{
	port_outb(port + 1, 0x00);
	port_outb(port + 3, 0x80);
	port_outb(port + 0, 0x01); /* divisor low = 1  -> 115200 baud */
	port_outb(port + 1, 0x00); /* divisor high = 0 */
	port_outb(port + 3, 0x03);
	port_outb(port + 2, 0xC7);
	port_outb(port + 4, 0x03);
}

void serial_putc(unsigned int port, char c)
{
	/* Mirror the debug console to the network (netlog; a no-op until the
	 * mirror is enabled and the NIC is up). */
	if (port == SERIAL_COM1)
		netlog_char(c);

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
	(void)port;
	return serial_ring_count() > 0;
}

void serial_poll(void)
{
	unsigned char c;
	while ((port_inb(SERIAL_COM1 + 5) & 0x01) != 0) {
		c = (unsigned char)port_inb(SERIAL_COM1);
		serial_ring_push(c);
	}
}

int serial_ring_getc(void)
{
	unsigned char c;
	if (serial_ring_pop(&c))
		return (int)c;
	return 0;
}

int serial_ring_wait(unsigned int timeout_ms, unsigned char *out)
{
	unsigned int start = get_ticks_ms();
	int got = 0;

	/* A kexec/payload receive can take minutes over 115200 baud.  Do not
	 * keep the big kernel lock for that: the WM, terminals and the other
	 * CPU's syscalls must keep running while we wait for bytes. */
	bkl_pause();
	while (get_ticks_ms() - start < timeout_ms) {
		if (serial_ring_pop(out)) {
			got = 1;
			break;
		}
		__asm__ volatile ("hlt");
	}
	bkl_resume();
	return got;
}
