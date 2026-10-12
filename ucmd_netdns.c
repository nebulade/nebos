/* netdns: ring-3 command.  Show or set the DNS resolver. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	unsigned char dns[4], gw[4];

	if (!arg || !arg[0]) {
		u_syscall1(SYS_NET_GET_DNS, (int)dns);
		if (dns[0] || dns[1] || dns[2] || dns[3]) {
			u_puts("dns=");
			u_put_ip(dns);
			u_putc('\n');
			u_exit(0);
		}
		u_syscall1(SYS_NET_GET_GW, (int)gw);
		u_puts("dns: unset");
		if (gw[0] || gw[1] || gw[2] || gw[3]) {
			u_puts(" (fallback: gateway ");
			u_put_ip(gw);
			u_putc(')');
		}
		u_putc('\n');
		u_exit(0);
	}

	if (u_parse_ip(arg, dns) != 0) {
		u_puts("netdns: usage netdns [a.b.c.d]\n");
		u_exit(1);
	}

	u_syscall1(SYS_NET_SET_DNS, (int)dns);
	u_puts("dns set to ");
	u_put_ip(dns);
	u_putc('\n');
	u_exit(0);
}
