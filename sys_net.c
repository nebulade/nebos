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
#include "netkexec.h"
#include "netlog.h"
#include "ntp.h"
#include "process.h"
#include "tcp.h"
#include "timer.h"

/* One TCP connection at a time (the stack is single-connection, and the
 * receiver is owned by the process using it). */
static struct tcp_conn tcp;

/* Accepted server connection (NULL until SYS_TCP_ACCEPT returns one). */
static struct tcp_conn *srv_conn;

/* Reply buffer for ARP/ICMP answered while listening (the rx scratch is
 * occupied by the received frame). */
static unsigned char srv_reply[NETDEV_FRAME_MAX];

/* Long waits inside the stack (ARP/DNS/DHCP/TCP) spin on get_ticks_ms(), so
 * they need interrupts: the syscall stub entered with them off, and a frozen
 * tick counter would never satisfy a deadline.  The netdev ownership already
 * serializes net users, so running with IRQs on (and being preempted) is
 * safe. */
static void net_enter(void)
{
	/* ARP/DNS/DHCP/TCP waits spin on the PIT and can take seconds.  Give
	 * the other CPU's syscalls (and the WM) the kernel while we wait; the
	 * NIC ownership still serializes every stack user. */
	bkl_pause();
	__asm__ volatile ("sti");
}

static void net_exit(void)
{
	__asm__ volatile ("cli");
	bkl_resume();
}

static unsigned int caller_pid(void)
{
	return current_proc()->pid;
}

/* Called from the scheduler's reap loop for every dead process. */
void sys_net_reap(unsigned int pid)
{
	if (netdev_owner() == pid) {
		netdev_release_all();
		tcp_srv_reset();
		srv_conn = 0;
	}
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
	/* print_info reads the PHY via MDIC and can wait on the PHY/ME
	 * semaphore; the syscall stub entered with interrupts off. */
	net_enter();
	nd->print_info();
	net_exit();
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
	net_enter();
	nd->dump_regs();
	net_exit();
	return 0;
}

int sys_net_reset(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->reset)
		return -1;
	/* Own the receiver for the whole reset, so the netlog kworker cannot
	 * submit frames while the MAC and rings are being reinitialised. */
	netdev_acquire();
	/* The reset waits on ticks (reset timeouts, PHY autoneg); the syscall
	 * entered with interrupts off. */
	net_enter();
	nd->reset();
	net_exit();
	netdev_release();
	return 0;
}

/* net100 fallback: force 100M or restore auto.  The driver drops and
 * renegotiates the link, which can take a few seconds; own the receiver so
 * the background watcher cannot poll frames while the link is down. */
int sys_net_speed(unsigned int mbps)
{
	const struct netdev *nd = netdev_active();
	int r;

	if (!nd || !nd->set_speed)
		return -1;
	if (sys_net_acquire() != 0)
		return -2;
	net_enter();
	r = nd->set_speed(mbps);
	net_exit();
	sys_net_release();
	return r;
}

int sys_net_mac_set(const unsigned char mac[6])
{
	const struct netdev *nd = netdev_active();

	if (!nd || !nd->set_mac)
		return -1;
	if (sys_net_acquire() != 0)
		return -2;
	nd->set_mac(mac);
	sys_net_release();
	return 0;
}

int sys_dhcp_run(unsigned int timeout_ms)
{
	int r;

	net_enter();
	r = dhcp_run(timeout_ms);
	net_exit();
	return r;
}

int sys_dhcp_info(unsigned char server[4], unsigned int *lease)
{
	return dhcp_last_info(server, lease);
}

int sys_dns_resolve(const char *name, unsigned char ip[4])
{
	int r;

	if (!owns_nic())
		return -1;
	net_enter();
	r = dns_resolve(name, ip);
	net_exit();
	return r;
}

/* SNTP sync for ring-3 callers.  The service owns the receiver for the
 * duration (unless the caller already does), so settime does not have to
 * take SYS_NET_ACQUIRE itself.  A second network user gets SYS_NTP_EBUSY. */
int sys_ntp_sync(const char *server, unsigned int timeout_ms,
		 struct sys_datetime *out)
{
	unsigned int pid = caller_pid();
	int acquired = 0;
	int r;

	if (!netdev_active())
		return SYS_NTP_ENONIC;
	if (netdev_in_use()) {
		if (netdev_owner() != pid)
			return SYS_NTP_EBUSY;
	} else {
		netdev_acquire();
		acquired = 1;
	}

	net_enter();
	r = ntp_sync(server, timeout_ms, out);
	net_exit();
	if (acquired)
		netdev_release();
	return r;
}

/* ---- TCP client ---- */

int sys_tcp_connect(const unsigned char mac[6], const unsigned char ip[4],
		    unsigned int port)
{
	int r;

	if (!owns_nic())
		return -3;
	net_enter();
	r = tcp_connect(mac, ip, port, &tcp);
	net_exit();
	return r;
}

int sys_tcp_peer_mss(void)
{
	return (int)tcp.peer_mss;
}

int sys_tcp_send(const void *buf, unsigned int len)
{
	int r;

	if (!owns_nic())
		return -3;
	net_enter();
	r = tcp_send(&tcp, buf, len);
	net_exit();
	return r;
}

int sys_tcp_recv(void *buf, unsigned int max, unsigned int timeout_ms)
{
	int r;

	if (!owns_nic())
		return -2;
	net_enter();
	r = tcp_recv(&tcp, buf, max, get_ticks_ms() + timeout_ms);
	net_exit();
	return r;
}

void sys_tcp_close(void)
{
	if (!owns_nic())
		return;
	net_enter();
	tcp_close(&tcp);
	net_exit();
}

/* ---- TCP server ---- */

int sys_tcp_listen(unsigned int port)
{
	if (!owns_nic() || port == 0 || port > 65535)
		return -1;
	net_enter();
	tcp_srv_init(port);
	net_exit();
	srv_conn = 0;
	return 0;
}

/* Poll the receiver for new connections.  Also answers ARP/ICMP and hands a
 * netkexec header back to the kworker (the server exits, the watcher picks
 * the transfer up without waiting for the host's retry). */
int sys_tcp_accept(void)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rx;
	struct tcp_conn *c;
	int i, len;

	if (!nd || !owns_nic())
		return -2;
	rx = netdev_rx_scratch();

	net_enter();
	for (i = 0; i < 16; i++) {
		len = nd->rx(rx, NETDEV_FRAME_MAX);
		if (len < 0)
			break;
		if (tcp_srv_input(rx, (unsigned int)len))
			continue;
		if (netkexec_capture_frame(rx, (unsigned int)len)) {
			net_exit();
			return -3;
		}
		{
			int rlen = net_build_reply(rx, (unsigned int)len,
						   srv_reply,
						   sizeof(srv_reply));
			if (rlen > 0)
				nd->tx(srv_reply, (unsigned int)rlen);
		}
	}
	c = tcp_srv_accept();
	net_exit();
	if (!c)
		return 0;
	srv_conn = c;
	return 1;
}

int sys_tcp_srv_recv(void *buf, unsigned int max, unsigned int timeout_ms)
{
	int r;

	if (!owns_nic() || !srv_conn)
		return -2;
	net_enter();
	r = tcp_recv(srv_conn, buf, max, get_ticks_ms() + timeout_ms);
	net_exit();
	return r;
}

int sys_tcp_srv_send(const void *buf, unsigned int len)
{
	int r;

	if (!owns_nic() || !srv_conn)
		return -3;
	net_enter();
	r = tcp_srv_send(srv_conn, buf, len);
	net_exit();
	return r;
}

void sys_tcp_srv_close(void)
{
	if (owns_nic() && srv_conn) {
		net_enter();
		tcp_srv_close(srv_conn);
		net_exit();
	}
	srv_conn = 0;
}

/* Peer of the accepted connection (for the server's connection log). */
int sys_tcp_peer(unsigned char ip[4], unsigned int *port)
{
	unsigned int i;

	if (!srv_conn)
		return -1;
	if (ip)
		for (i = 0; i < 4; i++)
			ip[i] = srv_conn->dst_ip[i];
	if (port)
		*port = srv_conn->dport;
	return 0;
}

int sys_net_next_hop(const unsigned char dst[4], unsigned char hop[4])
{
	net_next_hop(dst, hop);
	return 0;
}

int sys_net_arp(const unsigned char ip[4], unsigned char mac[6])
{
	int r;

	if (!owns_nic())
		return -2;
	net_enter();
	r = net_arp_resolve(ip, mac);
	net_exit();
	return r;
}

int sys_net_configured(void)
{
	return net_is_configured();
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
