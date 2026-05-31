#include "io.h"
#include "serial.h"
#include "timer.h"
#include "idle.h"

void program_serial_loop_main(const char *arg)
{
	unsigned int port = SERIAL_COM1;
	unsigned int last_send;
	const char msg[] = "hello serial\r\n";

	(void)arg;

	serial_init(port);
	last_send = get_ticks_ms();

	for (;;) {
		unsigned int now = get_ticks_ms();

		if (now - last_send >= 1000) {
			last_send = now;
			serial_puts(port, msg);
		}

		{
			int c = serial_ring_getc();
			if (c)
				io_putchar((char)c);
		}

		cpu_idle();
	}
}
