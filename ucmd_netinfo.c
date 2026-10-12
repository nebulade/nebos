/* netinfo: ring-3 command.  NIC, addresses and DHCP lease. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	struct sys_netinfo n;
	unsigned char srv[4];
	unsigned int lease = 0;

	(void)arg;
	if (u_syscall1(SYS_NET_INFO, (int)&n) != 1) {
		u_puts("netinfo: no NIC found\n");
		u_exit(0);
	}

	u_puts("netdev: ");
	u_puts(n.name);
	u_puts(" (");
	u_puts(n.chip);
	u_puts(")\n");
	u_syscall0(SYS_NET_PRINT_INFO);

	u_puts("ip=");
	u_put_ip(n.ip);
	if (n.mask[0] || n.mask[1] || n.mask[2] || n.mask[3]) {
		u_puts("/");
		u_put_ip(n.mask);
	}
	u_putc('\n');
	if (n.gw[0] || n.gw[1] || n.gw[2] || n.gw[3]) {
		u_puts("gw=");
		u_put_ip(n.gw);
		u_putc('\n');
	}
	if (n.dns[0] || n.dns[1] || n.dns[2] || n.dns[3]) {
		u_puts("dns=");
		u_put_ip(n.dns);
		u_putc('\n');
	}
	if (u_syscall2(SYS_DHCP_INFO, (int)srv, (int)&lease) == 1) {
		u_puts("dhcp: server ");
		u_put_ip(srv);
		u_puts(" lease ");
		u_putdec(lease);
		u_puts("s\n");
	}
	u_exit(0);
}
