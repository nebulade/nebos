/* netreset: ring-3 command.  Full NIC re-init. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	struct sys_netinfo n;

	(void)arg;
	if (u_syscall1(SYS_NET_INFO, (int)&n) != 1) {
		u_puts("netreset: no NIC found\n");
		u_exit(1);
	}
	if (u_syscall0(SYS_NET_RESET) != 0) {
		u_puts("netreset: not supported by ");
		u_puts(n.name);
		u_putc('\n');
		u_exit(1);
	}
	u_puts("netreset: re-initializing ");
	u_puts(n.name);
	u_puts("...\n");
	u_syscall0(SYS_NET_PRINT_INFO);
	u_exit(0);
}
