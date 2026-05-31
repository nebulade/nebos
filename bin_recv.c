/* Simple binary receiver: size (4 bytes BE) + data + checksum (1 byte).
   Returns bytes received, or -1 on error/timeout.
   Prints '.' via io_putchar for each 512-byte chunk received. */

#include "bin_recv.h"
#include "serial.h"
#include "io.h"
#include "timer.h"

#define TIMEOUT_MS 5000

static int wait_byte(unsigned int port, unsigned char *out, unsigned int timeout_ms)
{
	unsigned int start = get_ticks_ms();
	while (get_ticks_ms() - start < timeout_ms) {
		if (serial_has_data(port)) {
			*out = (unsigned char)serial_getc_nb(port);
			return 1;
		}
	}
	return 0;
}

int bin_recv(unsigned int port, void *buf, unsigned int max_size)
{
	unsigned char *p = (unsigned char *)buf;
	unsigned int total = 0;
	unsigned char c;
	unsigned int i;
	unsigned int size = 0;
	unsigned char checksum = 0;
	unsigned char recv_sum = 0;

	/* Send 'R' to signal ready, wait for sender to start. */
	serial_putc(port, 'R');

	/* Read 4-byte size (big-endian). */
	for (i = 0; i < 4; i++) {
		if (!wait_byte(port, &c, TIMEOUT_MS)) {
			io_putstr("timeout reading size\n");
			return -1;
		}
		size = (size << 8) | c;
	}

	if (size == 0 || size > max_size) {
		io_putstr("bad size\n");
		return -1;
	}

	/* Read data bytes. */
	for (i = 0; i < size; i++) {
		if (!wait_byte(port, &c, TIMEOUT_MS)) {
			io_putstr("timeout at byte ");
			serial_putdec(port, i);
			io_putstr("\n");
			return -1;
		}
		p[i] = c;
		checksum += c;
		total++;
		if ((i + 1) % 512 == 0)
			io_putchar('.');
	}
	io_putchar('.');

	/* Read checksum byte. */
	if (!wait_byte(port, &recv_sum, TIMEOUT_MS)) {
		io_putstr("timeout reading checksum\n");
		return -1;
	}

	if (checksum != recv_sum) {
		io_putstr("checksum mismatch\n");
		return -1;
	}

	io_putchar('\n');
	return (int)total;
}
