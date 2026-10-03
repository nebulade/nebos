/* Serial receive command: receives a binary via serial into 0x31000. */

#include "io.h"
#include "serial.h"
#include "bin_recv.h"

#define PROGRAM_LOAD_ADDR 0x31000
#define PROGRAM_LOAD_MAX  0x10000

unsigned int serial_recv_size;

void program_serial_recv_main(const char *arg)
{
	int n;

	(void)arg;

	serial_recv_size = 0;

	n = bin_recv(SERIAL_COM1, (void *)PROGRAM_LOAD_ADDR, PROGRAM_LOAD_MAX);
	if (n < 0) {
		io_putstr("serial_recv: transfer failed\n");
		return;
	}

	serial_recv_size = (unsigned int)n;
	io_putstr("serial_recv: received ");
	serial_putdec(SERIAL_COM1, serial_recv_size);
	io_putstr(" bytes\n");
}
