/* host: ring-3 command.  One DNS A query; the resolver prints its answers. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	unsigned char ip[4];

	if (!arg || !arg[0]) {
		u_puts("host: usage host <name>\n");
		u_exit(1);
	}

	u_syscall0(SYS_NET_ACQUIRE);
	u_syscall2(SYS_DNS_RESOLVE, (int)arg, (int)ip);
	u_syscall0(SYS_NET_RELEASE);
	u_exit(0);
}
