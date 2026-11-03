/* net100: ring-3 command.  Force 100BASE-TX or restore auto/1G.
 *
 * This is the diagnostic fallback for a marginal 1000M pair/jack: 100BASE-TX
 * uses only two pairs, so a link that reports 1000M but loses DHCP/ARP
 * broadcasts can still be fully usable.  The driver keeps the setting until
 * `net100 off` or reboot. */

#include "ucmd_lib.h"

static int arg_is(const char *arg, const char *word)
{
	unsigned int i = 0;

	while (arg[i] && word[i] && arg[i] == word[i])
		i++;
	return arg[i] == '\0' && word[i] == '\0';
}

static void usage(void)
{
	u_puts("net100: usage net100 [on|off]  (on = force 100M, off = auto/1G)\n");
}

void _start(const char *arg)
{
	struct sys_netinfo n;
	unsigned int mbps;
	int r;

	if (!arg || !arg[0] || arg_is(arg, "on") || arg_is(arg, "100"))
		mbps = 100;
	else if (arg_is(arg, "off") || arg_is(arg, "auto") ||
		 arg_is(arg, "1000"))
		mbps = 0;
	else {
		usage();
		u_exit(1);
	}

	if (u_syscall1(SYS_NET_INFO, (int)&n) != 1) {
		u_puts("net100: no NIC found\n");
		u_exit(1);
	}

	r = u_syscall1(SYS_NET_SPEED, (int)mbps);
	if (r == -1) {
		u_puts("net100: speed control not supported by ");
		u_puts(n.name);
		u_putc('\n');
		u_exit(1);
	}
	if (r == -2) {
		u_puts("net100: NIC is busy (see ps)\n");
		u_exit(1);
	}

	u_puts(mbps == 100 ? "net100: forcing 100M\n"
			   : "net100: restoring auto/1G\n");
	u_syscall0(SYS_NET_PRINT_INFO);
	if (r == -3) {
		u_puts("net100: link did not come up; check the cable/port\n");
		u_exit(1);
	}
	if (r == -4) {
		u_puts("net100: link is up but not at 100M\n");
		u_exit(1);
	}
	u_exit(0);
}
