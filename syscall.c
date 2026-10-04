/* Syscall dispatcher: services int 0x80 requests from ring-3. */

#include "syscall.h"
#include "io.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

int syscall_dispatch(int num, int a1, int a2, int a3)
{
	(void)a2;
	(void)a3;

	switch (num) {
	case SYS_PUTSTR:
		io_putstr((const char *)a1);
		return 0;
	case SYS_GET_TICKS_MS:
		return (int)get_ticks_ms();
	case SYS_SLEEP_MS:
		process_sleep_ms((unsigned int)a1);
		return 0;
	case SYS_EXIT:
		process_exit();   /* never returns */
		return 0;
	default:
		return -1;
	}
}
