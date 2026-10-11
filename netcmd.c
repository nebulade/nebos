/* Network commands: netinfo, netrecv, netsend, netip, ping (responder) and
 * netping (active ARP + ICMP echo), over the netdev NIC layer. */

#include "io.h"
#include "timer.h"
#include "netdev.h"
#include "net.h"
#include "dhcp.h"
#include "netlog.h"

/* Shared frame buffers (file scope, not stack: the 4 KB process stack can't
   hold a full-size frame, and keeping them out of BSS-as-much-as-possible
   avoids bloating the kernel past its 0x42000 limit). Max Ethernet frame is
   1518 bytes (14 header + 1500 payload + 4 VLAN). */
#define NET_FRAME_MAX 1518
static unsigned char net_rx_buf[NET_FRAME_MAX];
static unsigned char net_reply_buf[NET_FRAME_MAX];

static void put_hex2(unsigned char v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
}

static void put_dec(unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		io_putchar('0');
		return;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		io_putchar(b[--i]);
}

static void put_mac(const unsigned char *m)
{
	unsigned int i;
	for (i = 0; i < 6; i++) {
		if (i)
			io_putchar(':');
		put_hex2(m[i]);
	}
}

static void put_ip(const unsigned char *ip)
{
	unsigned int i;
	for (i = 0; i < 4; i++) {
		if (i)
			io_putchar('.');
		put_dec(ip[i]);
	}
}

static int ip_eq(const unsigned char *a, const unsigned char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- netinfo ---- */

void program_netinfo_main(void)
{
	const struct netdev *nd = netdev_active();
	unsigned char ip[4], mask[4], gw[4], server[4];
	unsigned int lease = 0;

	if (!nd) {
		io_putstr("netinfo: no NIC found\n");
		return;
	}
	io_putstr("netdev: ");
	io_putstr(nd->name);
	io_putstr(" (");
	io_putstr(nd->chip);
	io_putstr(")\n");
	nd->print_info();

	net_get_ip(ip);
	net_get_mask(mask);
	net_get_gateway(gw);
	io_putstr("ip=");
	put_ip(ip);
	if (mask[0] || mask[1] || mask[2] || mask[3]) {
		io_putstr("/");
		put_ip(mask);
	}
	io_putchar('\n');
	if (gw[0] || gw[1] || gw[2] || gw[3]) {
		io_putstr("gw=");
		put_ip(gw);
		io_putchar('\n');
	}
	if (dhcp_last_info(server, &lease)) {
		io_putstr("dhcp: server ");
		put_ip(server);
		io_putstr(" lease ");
		put_dec(lease);
		io_putstr("s\n");
	}
}

/* ---- dhcp ---- */

void program_dhcp_main(void)
{
	switch (dhcp_run(4000)) {
	case 0:
		break;                  /* dhcp_run() printed the lease */
	case -1:
		/* message already printed */
		break;
	default:
		io_putstr("dhcp: no NIC\n");
		break;
	}
}

/* ---- netregs: driver register dump ---- */

void program_netregs_main(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd) {
		io_putstr("netregs: no NIC found\n");
		return;
	}
	if (!nd->dump_regs) {
		io_putstr("netregs: no register dump for ");
		io_putstr(nd->name);
		io_putchar('\n');
		return;
	}
	nd->dump_regs();
}

/* ---- netrecv ---- */

void program_netrecv_main(void)
{
	const struct netdev *nd = netdev_active();
	unsigned int deadline, count = 0;

	if (!nd) {
		io_putstr("netrecv: no NIC found\n");
		return;
	}

	io_putstr("netrecv: listening (8s)...\n");
	netdev_acquire();
	deadline = get_ticks_ms() + 8000;

	while (get_ticks_ms() < deadline) {
		int len = nd->rx(net_rx_buf, NET_FRAME_MAX);
		unsigned int i;

		if (len < 0)
			continue;

		net_decode_frame(net_rx_buf, (unsigned int)len);
		io_putchar('\n');

		/* Raw hex dump for closer inspection. */
		for (i = 0; i < (unsigned int)len; i++) {
			put_hex2(net_rx_buf[i]);
			io_putchar(' ');
			if ((i & 15) == 15)
				io_putchar('\n');
		}
		if (((unsigned int)len & 15) != 0)
			io_putchar('\n');

		if (++count >= 32)
			break;
	}

	io_putstr("netrecv: ");
	put_dec(count);
	io_putstr(" frame(s)\n");
	netdev_release();
}

/* ---- netsend ---- */

void program_netsend_main(void)
{
	const struct netdev *nd = netdev_active();
	unsigned char frame[64];
	unsigned char mac[6];
	unsigned int i;

	if (!nd) {
		io_putstr("netsend: no NIC found\n");
		return;
	}

	nd->get_mac(mac);

	/* Broadcast test frame: dst=ff:ff:ff:ff:ff:ff, src=our MAC,
	   ethertype 0x88B5 (local experimental), payload "NEBS". */
	for (i = 0; i < 6; i++)
		frame[i] = 0xFF;
	for (i = 0; i < 6; i++)
		frame[6 + i] = mac[i];
	frame[12] = 0x88;
	frame[13] = 0xB5;
	frame[14] = 'N';
	frame[15] = 'E';
	frame[16] = 'B';
	frame[17] = 'S';
	for (i = 18; i < 64; i++)
		frame[i] = 0x00;

	if (nd->tx(frame, sizeof(frame)) != 0) {
		io_putstr("netsend: transmit failed\n");
		return;
	}
	io_putstr("netsend: sent 64-byte broadcast frame\n");
}

/* ---- netip ---- */

void program_netip_main(const char *arg)
{
	unsigned char ip[4];

	if (arg[0] == '\0') {
		net_print_ip();
		io_putchar('\n');
		return;
	}

	if (net_parse_ip(arg, ip) != 0) {
		io_putstr("netip: usage netip <a.b.c.d>\n");
		return;
	}

	net_set_ip(ip);
	io_putstr("ip set to ");
	net_print_ip();
	io_putchar('\n');
}

/* ---- ping: answer ARP/ICMP for our IP (responder mode) ---- */

void program_ping_main(void)
{
	const struct netdev *nd = netdev_active();
	unsigned int deadline, count = 0;

	if (!nd) {
		io_putstr("ping: no NIC found\n");
		return;
	}

	io_putstr("ping: answering ARP/ICMP for 30s (ip=");
	net_print_ip();
	io_putstr(")...\n");
	netdev_acquire();

	deadline = get_ticks_ms() + 30000;
	while (get_ticks_ms() < deadline) {
		int len = nd->rx(net_rx_buf, NET_FRAME_MAX);
		int rlen;

		if (len < 0)
			continue;

		rlen = net_build_reply(net_rx_buf, (unsigned int)len,
				       net_reply_buf, NET_FRAME_MAX);
		if (rlen > 0) {
			if (nd->tx(net_reply_buf, (unsigned int)rlen) == 0) {
				io_putstr("reply: ");
				net_decode_frame(net_reply_buf, (unsigned int)rlen);
				io_putchar('\n');
				count++;
			}
		}
	}

	io_putstr("ping: done (");
	put_dec(count);
	io_putstr(" replies)\n");
	netdev_release();
}

/* ---- netlog: send a text line to the LAN as a UDP datagram ---- */

void program_netlog_main(const char *arg)
{
	static const unsigned char bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	unsigned char dst[4];
	unsigned int len = 0;

	if (arg[0] == '\0') {
		io_putstr("netlog: usage netlog <text>\n");
		return;
	}
	while (arg[len] && len < NET_UDP_MAX)
		len++;

	netlog_get_host(dst);
	if (net_udp_send(bcast_mac, dst, NETLOG_PORT, NETLOG_PORT,
			 arg, len) != 0) {
		io_putstr("netlog: send failed\n");
		return;
	}
	io_putstr("netlog: sent ");
	put_dec(len);
	io_putstr(" bytes to UDP port ");
	put_dec(NETLOG_PORT);
	io_putchar('\n');
}

/* ---- netconsole: the debug-output mirror (see netlog.c) ---- */

static int arg_is(const char *arg, const char *word)
{
	unsigned int i = 0;

	while (arg[i] && word[i] && arg[i] == word[i])
		i++;
	return arg[i] == '\0' && word[i] == '\0';
}

void program_netconsole_main(const char *arg)
{
	unsigned char ip[4];

	if (arg[0] == '\0') {
		io_putstr("netconsole: mirror ");
		io_putstr(netlog_get_enabled() ? "on" : "off");
		io_putstr(", host ");
		netlog_get_host(ip);
		put_ip(ip);
		io_putstr(", udp port ");
		put_dec(NETLOG_PORT);
		io_putchar('\n');
		io_putstr("netconsole: usage netconsole on|off|host <a.b.c.d>\n");
		return;
	}

	if (arg_is(arg, "on")) {
		netlog_set_enabled(1);
		io_putstr("netconsole: mirror on\n");
		return;
	}
	if (arg_is(arg, "off")) {
		netlog_set_enabled(0);
		io_putstr("netconsole: mirror off\n");
		return;
	}
	if (arg[0] == 'h' && arg[1] == 'o' && arg[2] == 's' && arg[3] == 't') {
		const char *p = arg + 4;

		while (*p == ' ')
			p++;
		if (*p == '\0' || net_parse_ip(p, ip) != 0) {
			io_putstr("netconsole: usage netconsole host <a.b.c.d>\n");
			return;
		}
		netlog_set_host(ip);
		io_putstr("netconsole: host ");
		put_ip(ip);
		io_putchar('\n');
		return;
	}

	io_putstr("netconsole: usage netconsole on|off|host <a.b.c.d>\n");
}

/* ---- netreset: ask the driver for a full NIC re-init ---- */

void program_netreset_main(void)
{
	const struct netdev *nd = netdev_active();

	if (!nd) {
		io_putstr("netreset: no NIC found\n");
		return;
	}
	if (!nd->reset) {
		io_putstr("netreset: not supported by ");
		io_putstr(nd->name);
		io_putchar('\n');
		return;
	}
	io_putstr("netreset: re-initializing ");
	io_putstr(nd->name);
	io_putstr("...\n");
	nd->reset();
	nd->print_info();
}

/* ---- netloop: PHY loopback self-test (no cable/peer needed) ---- */

static void netloop_run(void)
{
	const struct netdev *nd = netdev_active();
	unsigned char frame[64], mac[6];
	unsigned int i, deadline;
	int got = 0;

	if (!nd) {
		io_putstr("netloop: no NIC found\n");
		return;
	}
	if (!nd->set_loopback) {
		io_putstr("netloop: not supported by ");
		io_putstr(nd->name);
		io_putchar('\n');
		return;
	}

	nd->get_mac(mac);
	for (i = 0; i < 6; i++)
		frame[i] = 0xFF;                      /* broadcast */
	for (i = 0; i < 6; i++)
		frame[6 + i] = mac[i];
	frame[12] = 0x88;
	frame[13] = 0xB6;                             /* local experimental */
	frame[14] = 'N'; frame[15] = 'E'; frame[16] = 'B';
	frame[17] = 'O'; frame[18] = 'S'; frame[19] = 'L';
	frame[20] = 'O'; frame[21] = 'O';
	for (i = 22; i < 64; i++)
		frame[i] = 0x00;

	io_putstr("netloop: PHY loopback on, sending...\n");
	nd->set_loopback(1);

	if (nd->tx(frame, sizeof(frame)) != 0) {
		io_putstr("netloop: transmit failed\n");
		nd->set_loopback(0);
		return;
	}

	deadline = get_ticks_ms() + 1000;
	while (get_ticks_ms() < deadline) {
		int len = nd->rx(net_rx_buf, NET_FRAME_MAX);

		if (len < 0)
			continue;
		if (len >= 22 &&
		    net_rx_buf[12] == 0x88 && net_rx_buf[13] == 0xB6 &&
		    net_rx_buf[14] == 'N' && net_rx_buf[15] == 'E' &&
		    net_rx_buf[16] == 'B' && net_rx_buf[17] == 'O' &&
		    net_rx_buf[18] == 'S') {
			got = 1;
			break;
		}
	}

	nd->set_loopback(0);

	io_putstr(got ? "netloop: PASS (frame came back)\n"
		      : "netloop: FAIL (no frame came back)\n");
}

void program_netloop_main(void)
{
	netdev_acquire();
	netloop_run();
	netdev_release();
}

/* ---- netping: active ARP + ICMP echo ---- */

#define NETPING_COUNT  4
#define NETPING_ID     0x4E42   /* "NB" */
#define NETPING_WAIT_MS 1000

static unsigned int netping_sent_ms[NETPING_COUNT];

/* ARP request "who has target? tell my_ip" -> 42-byte frame. */
static unsigned int build_arp_request(const unsigned char *my_mac,
				      const unsigned char *my_ip,
				      const unsigned char *target_ip,
				      unsigned char *out)
{
	unsigned char *a = out + 14;
	unsigned int i;

	for (i = 0; i < 6; i++)
		out[i] = 0xFF;                        /* broadcast */
	for (i = 0; i < 6; i++)
		out[6 + i] = my_mac[i];
	out[12] = 0x08;
	out[13] = 0x06;
	a[0] = 0; a[1] = 1;                           /* htype Ethernet */
	a[2] = 0x08; a[3] = 0;                        /* ptype IPv4 */
	a[4] = 6; a[5] = 4;
	a[6] = 0; a[7] = 1;                           /* opcode request */
	for (i = 0; i < 6; i++)                       /* sha */
		a[8 + i] = my_mac[i];
	for (i = 0; i < 4; i++)                       /* spa */
		a[14 + i] = my_ip[i];
	for (i = 0; i < 6; i++)                       /* tha */
		a[18 + i] = 0;
	for (i = 0; i < 4; i++)                       /* tpa */
		a[24 + i] = target_ip[i];
	return 42;
}

/* 1 + peer MAC if frame is an ARP reply from target_ip. */
static int parse_arp_reply(const unsigned char *f, unsigned int len,
			   const unsigned char *target_ip, unsigned char *mac)
{
	const unsigned char *a = f + 14;
	unsigned int i;

	if (len < 14 + 28)
		return 0;
	if ((((unsigned int)f[12] << 8) | f[13]) != ET_ARP)
		return 0;
	if ((((unsigned int)a[6] << 8) | a[7]) != 2)   /* opcode reply */
		return 0;
	if (a[4] != 6 || a[5] != 4)
		return 0;
	if (!ip_eq(a + 14, target_ip))                 /* sender IP == target */
		return 0;
	for (i = 0; i < 6; i++)
		mac[i] = a[8 + i];
	return 1;
}

/* ICMP echo request (14 + 20 + 16 bytes; the driver pads to 60). */
static unsigned int build_icmp_echo(const unsigned char *src_mac,
				    const unsigned char *dst_mac,
				    const unsigned char *src_ip,
				    const unsigned char *dst_ip,
				    unsigned short seq, unsigned char *out)
{
	unsigned char *ip = out + 14;
	unsigned char *ic = ip + 20;
	unsigned char *pl = ic + 8;
	unsigned short cs;
	unsigned int i;

	for (i = 0; i < 6; i++)
		out[i] = dst_mac[i];
	for (i = 0; i < 6; i++)
		out[6 + i] = src_mac[i];
	out[12] = 0x08;
	out[13] = 0x00;

	for (i = 0; i < 20; i++)
		ip[i] = 0;
	ip[0] = 0x45;
	ip[2] = 0; ip[3] = 36;                        /* total length */
	ip[8] = 64;                                   /* TTL */
	ip[9] = IPPROTO_ICMP;
	for (i = 0; i < 4; i++)
		ip[12 + i] = src_ip[i];
	for (i = 0; i < 4; i++)
		ip[16 + i] = dst_ip[i];
	cs = net_checksum(ip, 20);
	ip[10] = (unsigned char)(cs >> 8);
	ip[11] = (unsigned char)(cs & 0xFF);

	ic[0] = ICMP_ECHO_REQUEST;
	ic[1] = 0;
	ic[2] = 0; ic[3] = 0;
	ic[4] = (unsigned char)(NETPING_ID >> 8);
	ic[5] = (unsigned char)(NETPING_ID & 0xFF);
	ic[6] = (unsigned char)(seq >> 8);
	ic[7] = (unsigned char)(seq & 0xFF);
	pl[0] = 'n'; pl[1] = 'e'; pl[2] = 'b'; pl[3] = 'o';
	pl[4] = 's'; pl[5] = 0; pl[6] = 0; pl[7] = 0;
	cs = net_checksum(ic, 16);
	ic[2] = (unsigned char)(cs >> 8);
	ic[3] = (unsigned char)(cs & 0xFF);

	return 14 + 20 + 16;
}

/* 1 + ICMP echo-reply seq for our IP from peer_ip. */
static int parse_icmp_reply(const unsigned char *f, unsigned int len,
			    const unsigned char *my_ip,
			    const unsigned char *peer_ip,
			    unsigned short *seq)
{
	const unsigned char *ip = f + 14;
	const unsigned char *ic;
	unsigned int ihl;
	unsigned short ip_len, id;

	if (len < 14 + 20)
		return 0;
	if ((((unsigned int)f[12] << 8) | f[13]) != ET_IPV4)
		return 0;
	if ((ip[0] >> 4) != 4)
		return 0;
	ihl = (unsigned int)(ip[0] & 0x0F) * 4;
	if (ihl < 20 || ip[9] != IPPROTO_ICMP)
		return 0;
	ip_len = ((unsigned short)ip[2] << 8) | ip[3];
	if (ip_len < ihl + 8 || (unsigned int)ip_len > len - 14)
		return 0;
	if (!ip_eq(ip + 12, peer_ip))                 /* src == peer */
		return 0;
	if (!ip_eq(ip + 16, my_ip))                   /* dst == us */
		return 0;
	ic = ip + ihl;
	if (ic[0] != ICMP_ECHO_REPLY || ic[1] != 0)
		return 0;
	id = ((unsigned short)ic[4] << 8) | ic[5];
	if (id != NETPING_ID)
		return 0;
	*seq = ((unsigned short)ic[6] << 8) | ic[7];
	if (*seq < 1 || *seq > NETPING_COUNT)
		return 0;
	return 1;
}

/* Resolve target_ip's MAC by ARP (3 tries, 1 s each).
 * Returns 0 = resolved, -1 = no reply, -2 = transmit failed.
 * Prints the frames seen while waiting (capped) so a silent peer, a filtered
 * reply and a reply we refuse to parse can be told apart on screen. */
static int netping_arp(const struct netdev *nd, const unsigned char *my_mac,
		       const unsigned char *my_ip, const unsigned char *target_ip,
		       unsigned char *target_mac)
{
	unsigned char frame[64];
	unsigned int tries, seen = 0;

	for (tries = 0; tries < 3; tries++) {
		unsigned int deadline = get_ticks_ms() + NETPING_WAIT_MS;
		unsigned int flen = build_arp_request(my_mac, my_ip, target_ip, frame);

		if (nd->tx(frame, flen) != 0)
			return -2;
		io_putstr("netping: ARP request sent (try ");
		put_dec(tries + 1);
		io_putstr("/3)\n");
		while (get_ticks_ms() < deadline) {
			int len = nd->rx(net_rx_buf, NET_FRAME_MAX);

			if (len < 0)
				continue;
			if (seen < 8) {
				io_putstr("netping: rx ");
				net_decode_frame(net_rx_buf, (unsigned int)len);
				io_putchar('\n');
				seen++;
			}
			if (parse_arp_reply(net_rx_buf, (unsigned int)len,
					    target_ip, target_mac))
				return 0;
		}
	}
	io_putstr("netping: saw ");
	put_dec(seen);
	io_putstr(" frame(s) while waiting\n");
	return -1;
}

static void netping_run(const char *arg)
{
	const struct netdev *nd;
	unsigned char target[4], my_ip[4], my_mac[6], target_mac[6];
	unsigned int i, replies = 0;

	if (arg[0] == '\0' || net_parse_ip(arg, target) != 0) {
		io_putstr("netping: usage netping <a.b.c.d>\n");
		return;
	}

	nd = netdev_active();
	if (!nd) {
		io_putstr("netping: no NIC found\n");
		return;
	}
	nd->get_mac(my_mac);
	net_get_ip(my_ip);

	/* With a mask and gateway known, off-subnet targets go via the router;
	 * we can only ARP for the next hop. */
	{
		unsigned char mask[4], gw[4], hop[4];
		unsigned int j, off = 0;

		net_get_mask(mask);
		net_get_gateway(gw);
		for (j = 0; j < 4; j++)
			hop[j] = target[j];
		if ((mask[0] || mask[1] || mask[2] || mask[3]) &&
		    (gw[0] || gw[1] || gw[2] || gw[3])) {
			for (j = 0; j < 4; j++) {
				if ((target[j] & mask[j]) != (my_ip[j] & mask[j]))
					off = 1;
			}
		}
		if (off) {
			for (j = 0; j < 4; j++)
				hop[j] = gw[j];
			io_putstr("netping: ");
			put_ip(target);
			io_putstr(" via ");
			put_ip(hop);
			io_putchar('\n');
		}

		{
			int r = netping_arp(nd, my_mac, my_ip, hop, target_mac);

			if (r == -2) {
				io_putstr("netping: transmit failed (see netregs)\n");
				return;
			}
			if (r != 0) {
				io_putstr("netping: no ARP reply from ");
				put_ip(hop);
				io_putchar('\n');
				return;
			}
		}
	}
	io_putstr("netping: ");
	put_ip(target);
	io_putstr(" is-at ");
	put_mac(target_mac);
	io_putchar('\n');

	for (i = 0; i < NETPING_COUNT; i++) {
		unsigned char frame[64];
		unsigned int flen, deadline;
		int got = 0;

		flen = build_icmp_echo(my_mac, target_mac, my_ip, target,
				       (unsigned short)(i + 1), frame);
		netping_sent_ms[i] = get_ticks_ms();
		if (nd->tx(frame, flen) != 0) {
			io_putstr("netping: transmit failed\n");
			return;
		}

		deadline = get_ticks_ms() + NETPING_WAIT_MS;
		while (get_ticks_ms() < deadline && !got) {
			unsigned short seq;
			int len = nd->rx(net_rx_buf, NET_FRAME_MAX);
			unsigned int rtt;

			if (len < 0)
				continue;
			if (!parse_icmp_reply(net_rx_buf, (unsigned int)len,
					      my_ip, target, &seq))
				continue;
			rtt = get_ticks_ms() - netping_sent_ms[seq - 1];
			io_putstr("reply: seq=");
			put_dec(seq);
			io_putstr(" rtt=");
			if (rtt == 0)
				io_putstr("<1");
			else
				put_dec(rtt);
			io_putstr("ms\n");
			got = 1;
			replies++;
		}
		if (!got) {
			io_putstr("netping: timeout, seq=");
			put_dec(i + 1);
			io_putchar('\n');
		}
	}

	io_putstr("netping: ");
	put_dec(replies);
	io_putstr("/");
	put_dec(NETPING_COUNT);
	io_putstr(" replies from ");
	put_ip(target);
	io_putchar('\n');
}

void program_netping_main(const char *arg)
{
	netdev_acquire();
	netping_run(arg);
	netdev_release();
}
