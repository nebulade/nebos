/* netmac: ring-3 diagnostic.  Temporarily override the NIC's MAC address.
 *
 * Used to test whether a switch/repeater is bound to a particular MAC: set
 * the board to the ION's MAC (with the ION powered off), then run dhcp/ping.
 * The setting is RAM-only and disappears on reboot. */

#include "ucmd_lib.h"

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_mac(const char *s, unsigned char mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		int hi = hexval(s[0]);
		int lo = hexval(s[1]);

		if (hi < 0 || lo < 0)
			return -1;
		mac[i] = (unsigned char)((hi << 4) | lo);
		s += 2;
		if (i != 5) {
			if (*s != ':' && *s != '-')
				return -1;
			s++;
		}
	}
	return *s == '\0' ? 0 : -1;
}

void _start(const char *arg)
{
	struct sys_netinfo n;
	unsigned char old[6], mac[6];

	if (!arg || !arg[0]) {
		u_puts("netmac: usage netmac <xx:xx:xx:xx:xx:xx>\n");
		u_exit(1);
	}
	if (parse_mac(arg, mac) != 0) {
		u_puts("netmac: invalid MAC address\n");
		u_exit(1);
	}
	if (u_syscall1(SYS_NET_INFO, (int)&n) != 1) {
		u_puts("netmac: no NIC found\n");
		u_exit(1);
	}

	u_syscall1(SYS_NET_MAC, (int)old);
	if (u_syscall1(SYS_NET_MAC_SET, (int)mac) != 0) {
		u_puts("netmac: NIC busy or MAC override unsupported\n");
		u_exit(1);
	}

	u_puts("netmac: ");
	u_put_mac(old);
	u_puts(" -> ");
	u_put_mac(mac);
	u_puts("\nnetmac: RAM-only; reboot restores the factory MAC\n");
	u_syscall0(SYS_NET_PRINT_INFO);
	u_exit(0);
}
