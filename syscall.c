/* Syscall dispatcher: services int 0x80 requests from ring-3. */

#include "syscall.h"
#include "idle.h"
#include "io.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

int syscall_dispatch(int num, int a1, int a2, int a3)
{
	(void)a2;
	(void)a3;

	serial_puts(SERIAL_COM1, "syscall ");
	serial_putdec(SERIAL_COM1, (unsigned int)num);
	serial_puts(SERIAL_COM1, "\n");

	switch (num) {
	case SYS_PUTSTR:
		io_putstr((const char *)a1);
		return 0;
	case SYS_GET_TICKS_MS:
		return (int)get_ticks_ms();
	case SYS_SLEEP_MS: {
		unsigned int start = get_ticks_ms();
		unsigned int ms = (unsigned int)a1;
		while (get_ticks_ms() - start < ms)
			cpu_idle();
		return 0;
	}
	case SYS_EXIT:
		process_exit();   /* never returns */
		return 0;
	default:
		return -1;
	}
}
