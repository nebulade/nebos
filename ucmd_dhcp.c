/* dhcp: ring-3 command.  Re-run the DHCP client; dhcp.c prints the exchange. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	int r;

	(void)arg;
	r = u_syscall1(SYS_DHCP_RUN, 4000);
	if (r == -2)
		u_puts("dhcp: no NIC\n");
	else if (r == -3)
		u_puts("dhcp: TX failed (try netreset)\n");
	/* r == 0: dhcp_run() printed the lease; r == -1: it printed its own
	 * failure line. */
	u_exit(r == 0 ? 0 : 1);
}
