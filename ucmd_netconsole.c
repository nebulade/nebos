/* netconsole: ring-3 command.  The debug-output mirror's configuration. */

#include "ucmd_lib.h"

#define NETLOG_PORT 19290

static int arg_is(const char *arg, const char *word)
{
	unsigned int i = 0;

	while (arg[i] && word[i] && arg[i] == word[i])
		i++;
	return arg[i] == '\0' && word[i] == '\0';
}

static void usage(void)
{
	u_puts("netconsole: usage netconsole on|off|host <a.b.c.d>\n");
}

void _start(const char *arg)
{
	struct sys_netinfo n;
	unsigned char ip[4];

	if (!arg || !arg[0]) {
		u_syscall1(SYS_NET_INFO, (int)&n);
		u_puts("netconsole: mirror ");
		u_puts(n.mirror_on ? "on" : "off");
		u_puts(", host ");
		u_put_ip(n.mirror_host);
		u_puts(", udp port ");
		u_putdec(NETLOG_PORT);
		u_putc('\n');
		usage();
		u_exit(1);
	}

	if (arg_is(arg, "on")) {
		u_syscall2(SYS_NETLOG_CONF, 1, 0);
		u_puts("netconsole: mirror on\n");
		u_exit(0);
	}
	if (arg_is(arg, "off")) {
		u_syscall2(SYS_NETLOG_CONF, 0, 0);
		u_puts("netconsole: mirror off\n");
		u_exit(0);
	}
	if (arg[0] == 'h' && arg[1] == 'o' && arg[2] == 's' && arg[3] == 't') {
		const char *p = arg + 4;

		while (*p == ' ')
			p++;
		if (*p == '\0' || u_parse_ip(p, ip) != 0) {
			usage();
			u_exit(1);
		}
		u_syscall2(SYS_NETLOG_CONF, -1, (int)ip);
		u_puts("netconsole: host ");
		u_put_ip(ip);
		u_putc('\n');
		u_exit(0);
	}

	usage();
	u_exit(1);
}
