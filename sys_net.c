/* Network service for ring-3 commands (see syscall.h for the ABI).
 *
 * The stack (net.c/dns.c/tcp.c) and the NIC drivers stay in the kernel; user
 * commands get the configuration, the DNS/DHCP clients and the raw frame
 * paths, so the packet-level commands (ping, pong, netrecv, ...) port over
 * their existing frame code.  The receiver is owned by one process at a time
 * (netdev_acquire/release): the background kexec watcher pauses while a
 * command owns it, and the reaper force-releases it if that process dies
 * (sys_net_reap). */

#include "syscall.h"
#include "dhcp.h"
#include "dns.h"
#include "io.h"
#include "net.h"
#include "netdev.h"
#include "netlog.h"
#include "process.h"

static unsigned int caller_pid(void)
{
	return procs[current_pid].pid;
}

/* Called from the scheduler's reap loop for every dead process. */
void sys_net_reap(unsigned int pid)
{
	if (netdev_owner() == pid)
		netdev_release_all();
}

int sys_net_info(struct sys_netinfo *out)
{
	const struct netdev *nd = netdev_active();
	unsigned int i;

	for (i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;
	out->mirror_on = netlog_get_enabled() ? 1 : 0;
	netlog_get_host(out->mirror_host);
	if (!nd)
		return 0;

	out->present = 1;
	if (nd->get_mac)
		nd->get_mac(out->mac);
	net_get_ip(out->ip);
	net_get_mask(out->mask);
	net_get_gateway(out->gw);
	net_get_dns(out->dns);
	for (i = 0; nd->name[i] && i + 1 < sizeof(out->name); i++)
		out->name[i] = nd->name[i];
	for (i = 0; nd->chip[i] && i + 1 < sizeof(out->chip); i++)
		out->chip[i] = nd->chip[i];
	return 1;
}

int sys_net_print_info(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->print_info)
		return -1;
	nd->print_info();
	return 0;
}

/* The owner must be the caller: the syscall runs in its process context. */
static int owns_nic(void)
{
	return netdev_owner() == caller_pid();
}

int sys_net_acquire(void)
{
	if (owns_nic())
		return 0;
	if (netdev_in_use())
		return -1;
	netdev_acquire();
	return 0;
}

int sys_net_release(void)
{
	if (owns_nic())
		netdev_release();
	return 0;
}

int sys_net_mac(unsigned char mac[6])
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->get_mac)
		return -1;
	nd->get_mac(mac);
	return 0;
}

void sys_net_get_ip(unsigned char ip[4])       { net_get_ip(ip); }
void sys_net_set_ip(const unsigned char ip[4]) { net_set_ip(ip); }
void sys_net_get_mask(unsigned char m[4])      { net_get_mask(m); }
void sys_net_get_gw(unsigned char gw[4])       { net_get_gateway(gw); }
void sys_net_get_dns(unsigned char d[4])       { net_get_dns(d); }
void sys_net_set_dns(const unsigned char d[4]) { net_set_dns(d); }

int sys_net_tx(const unsigned char *buf, unsigned int len)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !owns_nic())
		return -1;
	return nd->tx(buf, len);
}

int sys_net_rx(unsigned char *buf, unsigned int max)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !owns_nic())
		return -1;
	return nd->rx(buf, max);
}

void sys_net_decode(const unsigned char *buf, unsigned int len)
{
	net_decode_frame(buf, len);
}

/* The reply buffer is always a full frame (the callers carry one). */
int sys_net_reply(const unsigned char *in, unsigned int len, unsigned char *out)
{
	if (!owns_nic())
		return -1;
	return net_build_reply(in, len, out, NETDEV_FRAME_MAX);
}

int sys_net_loopback(int on)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->set_loopback)
		return -1;
	return nd->set_loopback(on);
}

int sys_net_regs(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->dump_regs)
		return -1;
	nd->dump_regs();
	return 0;
}

int sys_net_reset(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->reset)
		return -1;
	nd->reset();
	return 0;
}

int sys_dhcp_run(unsigned int timeout_ms)
{
	return dhcp_run(timeout_ms);
}

int sys_dhcp_info(unsigned char server[4], unsigned int *lease)
{
	return dhcp_last_info(server, lease);
}

int sys_dns_resolve(const char *name, unsigned char ip[4])
{
	if (!owns_nic())
		return -1;
	return dns_resolve(name, ip);
}

int sys_netlog(const char *text)
{
	static const unsigned char bcast_mac[6] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	unsigned char dst[4];
	unsigned int len = 0;

	while (text[len] && len < NET_UDP_MAX)
		len++;
	netlog_get_host(dst);
	return net_udp_send(bcast_mac, dst, NETLOG_PORT, NETLOG_PORT, text, len);
}

int sys_netlog_conf(int enabled, const unsigned char *ip)
{
	if (enabled >= 0)
		netlog_set_enabled(enabled);
	if (ip)
		netlog_set_host(ip);
	return 0;
}
