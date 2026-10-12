/* netlog: ring-3 command.  Send one text line as a UDP datagram. */

#include "ucmd_lib.h"

#define NETLOG_PORT 19290

void _start(const char *arg)
{
	unsigned int len = 0;

	if (!arg || !arg[0]) {
		u_puts("netlog: usage netlog <text>\n");
		u_exit(1);
	}
	while (arg[len])
		len++;

	if (u_syscall1(SYS_NETLOG, (int)arg) != 0) {
		u_puts("netlog: send failed\n");
		u_exit(1);
	}
	u_puts("netlog: sent ");
	u_putdec(len);
	u_puts(" bytes to UDP port ");
	u_putdec(NETLOG_PORT);
	u_putc('\n');
	u_exit(0);
}
