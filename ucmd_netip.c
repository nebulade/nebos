/* netip: ring-3 command.  Show or set the IPv4 address. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	unsigned char ip[4];

	if (!arg || !arg[0]) {
		u_syscall1(SYS_NET_GET_IP, (int)ip);
		u_put_ip(ip);
		u_putc('\n');
		u_exit(0);
	}

	if (u_parse_ip(arg, ip) != 0) {
		u_puts("netip: usage netip <a.b.c.d>\n");
		u_exit(1);
	}

	u_syscall1(SYS_NET_SET_IP, (int)ip);
	u_puts("ip set to ");
	u_syscall1(SYS_NET_GET_IP, (int)ip);
	u_put_ip(ip);
	u_putc('\n');
	u_exit(0);
}
