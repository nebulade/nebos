/* netregs: ring-3 command.  Driver register dump. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	struct sys_netinfo n;

	(void)arg;
	if (u_syscall1(SYS_NET_INFO, (int)&n) != 1) {
		u_puts("netregs: no NIC found\n");
		u_exit(1);
	}
	if (u_syscall0(SYS_NET_REGS) != 0) {
		u_puts("netregs: no register dump for ");
		u_puts(n.name);
		u_putc('\n');
		u_exit(1);
	}
	u_exit(0);
}
