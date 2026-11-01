/* Minimal DHCP client: see dhcp.h. */

#include "io.h"
#include "net.h"
#include "netdev.h"
#include "timer.h"
#include "dhcp.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2

#define DHCP_DISCOVER    1
#define DHCP_OFFER       2
#define DHCP_REQUEST     3
#define DHCP_ACK         5
#define DHCP_NAK         6

/* BOOTP header (236 bytes) + cookie + options, padded to a 300-byte DHCP
 * message (RFC 2131 minimum). */
#define DHCP_MSG_LEN     300
#define DHCP_HDR_LEN     236
#define DHCP_MAGIC_OFF   236
#define DHCP_OPT_OFF     240

static unsigned char msg[DHCP_MSG_LEN];
static unsigned char rxbuf[1520];

/* Last successful lease (for `netinfo`). */
static int have_info;
static unsigned char last_server[4];
static unsigned int last_lease;

static void put_ip(const unsigned char *ip)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		unsigned int v = ip[i];

		if (i)
			io_putchar('.');
		if (v >= 100) {
			io_putchar((char)('0' + v / 100));
			v %= 100;
			io_putchar((char)('0' + v / 10));
			v %= 10;
		} else if (v >= 10) {
			io_putchar((char)('0' + v / 10));
			v %= 10;
		}
		io_putchar((char)('0' + v));
	}
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

static unsigned int rd32be(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static void wr32be(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/* Build a DHCP message of the given type.  `requested` (may be NULL) goes
 * into option 50 so the router tends to hand back the address we ask for. */
static void dhcp_build(unsigned char type, const unsigned char *my_mac,
		       unsigned int xid, unsigned int secs,
		       const unsigned char *requested,
		       const unsigned char *server_id)
{
	unsigned int i, o;

	for (i = 0; i < DHCP_MSG_LEN; i++)
		msg[i] = 0;

	msg[0] = DHCP_OP_REQUEST;
	msg[1] = 1;                    /* htype: Ethernet */
	msg[2] = 6;                    /* hlen */
	msg[3] = 0;                    /* hops */
	wr32be(msg + 4, xid);
	msg[8] = (unsigned char)(secs >> 8);
	msg[9] = (unsigned char)secs;
	msg[10] = 0x80;                /* flags: broadcast replies */
	msg[11] = 0;
	/* ciaddr/yiaddr/siaddr/giaddr stay zero */
	for (i = 0; i < 6; i++)
		msg[28 + i] = my_mac[i];   /* chaddr */
	msg[DHCP_MAGIC_OFF + 0] = 0x63;
	msg[DHCP_MAGIC_OFF + 1] = 0x82;
	msg[DHCP_MAGIC_OFF + 2] = 0x53;
	msg[DHCP_MAGIC_OFF + 3] = 0x63;

	o = DHCP_OPT_OFF;
	msg[o++] = 53; msg[o++] = 1; msg[o++] = type;           /* message type */
	msg[o++] = 61; msg[o++] = 7; msg[o++] = 1;               /* client id */
	for (i = 0; i < 6; i++)
		msg[o++] = my_mac[i];
	{
		static const char hostname[] = "nebos";
		unsigned int n = sizeof(hostname) - 1;

		msg[o++] = 12;                                  /* host name */
		msg[o++] = (unsigned char)n;
		for (i = 0; i < n; i++)
			msg[o++] = (unsigned char)hostname[i];
	}
	msg[o++] = 55; msg[o++] = 4;                            /* param request */
	msg[o++] = 1; msg[o++] = 3; msg[o++] = 6; msg[o++] = 51;
	if (requested) {
		msg[o++] = 50; msg[o++] = 4;
		for (i = 0; i < 4; i++)
			msg[o++] = requested[i];
	}
	if (server_id) {
		msg[o++] = 54; msg[o++] = 4;
		for (i = 0; i < 4; i++)
			msg[o++] = server_id[i];
	}
	msg[o++] = 255;                                         /* end */
}

/* Parse a reply; returns 1 for "ours" and fills the option values. */
static int dhcp_parse(const unsigned char *frame, unsigned int len,
		      unsigned int xid, const unsigned char *my_mac,
		      unsigned char *type, unsigned char yiaddr[4],
		      unsigned char mask[4], unsigned char gw[4],
		      unsigned char dns[4],
		      unsigned int *lease, unsigned char server[4])
{
	const unsigned char *p;
	unsigned char s_ip[4], d_ip[4];
	unsigned int sport, dport, plen, i, o;

	if (!net_udp_parse(frame, len, s_ip, d_ip, &sport, &dport, &p, &plen))
		return 0;
	if (dport != DHCP_CLIENT_PORT)
		return 0;
	if (plen < DHCP_OPT_OFF || p[0] != DHCP_OP_REPLY)
		return 0;
	if (p[DHCP_MAGIC_OFF] != 0x63 || p[DHCP_MAGIC_OFF + 1] != 0x82 ||
	    p[DHCP_MAGIC_OFF + 2] != 0x53 || p[DHCP_MAGIC_OFF + 3] != 0x63)
		return 0;
	if (rd32be(p + 4) != xid)
		return 0;
	for (i = 0; i < 6; i++) {
		if (p[28 + i] != my_mac[i])
			return 0;
	}

	*type = 0;
	for (i = 0; i < 4; i++) {
		yiaddr[i] = p[16 + i];
		mask[i] = 0;
		gw[i] = 0;
		dns[i] = 0;
		server[i] = 0;
	}
	*lease = 0;

	/* Option walk. */
	o = DHCP_OPT_OFF;
	while (o + 1 < plen) {
		unsigned char code = p[o];
		unsigned int olen;

		if (code == 0) {        /* pad */
			o++;
			continue;
		}
		if (code == 255)        /* end */
			break;
		if (o + 1 >= plen)
			break;
		olen = p[o + 1];
		if (o + 2 + olen > plen)
			break;
		switch (code) {
		case 53:
			if (olen >= 1)
				*type = p[o + 2];
			break;
		case 1:
			if (olen >= 4)
				for (i = 0; i < 4; i++)
					mask[i] = p[o + 2 + i];
			break;
		case 3:
			if (olen >= 4)
				for (i = 0; i < 4; i++)
					gw[i] = p[o + 2 + i];
			break;
		case 6:                 /* DNS servers (first one wins) */
			if (olen >= 4)
				for (i = 0; i < 4; i++)
					dns[i] = p[o + 2 + i];
			break;
		case 51:
			if (olen >= 4)
				*lease = rd32be(p + o + 2);
			break;
		case 54:
			if (olen >= 4)
				for (i = 0; i < 4; i++)
					server[i] = p[o + 2 + i];
			break;
		default:
			break;
		}
		o += 2 + olen;
	}
	(void)sport;
	return *type != 0;
}

/* Wait for a reply of the wanted type until `deadline`. */
static int dhcp_wait(const struct netdev *nd, unsigned int xid,
		     const unsigned char *my_mac, unsigned int deadline,
		     unsigned char want, unsigned char yiaddr[4],
		     unsigned char mask[4], unsigned char gw[4],
		     unsigned char dns[4],
		     unsigned int *lease, unsigned char server[4])
{
	while ((int)(get_ticks_ms() - deadline) < 0) {
		unsigned char type;
		unsigned char ryi[4], rmask[4], rgw[4], rdns[4], rserver[4];
		unsigned int rlease;
		int len = nd->rx(rxbuf, sizeof(rxbuf));

		if (len < 0)
			continue;
		if (!dhcp_parse(rxbuf, (unsigned int)len, xid, my_mac, &type,
				ryi, rmask, rgw, rdns, &rlease, rserver))
			continue;
		if (type == DHCP_NAK)
			return -2;
		if (type != want)
			continue;
		{
			unsigned int i;

			for (i = 0; i < 4; i++) {
				yiaddr[i] = ryi[i];
				mask[i] = rmask[i];
				gw[i] = rgw[i];
				dns[i] = rdns[i];
				server[i] = rserver[i];
			}
		}
		*lease = rlease;
		return 0;
	}
	return -1;
}

int dhcp_run(unsigned int timeout_ms)
{
	static const unsigned char bcast_mac[6] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	static const unsigned char bcast_ip[4] = { 255, 255, 255, 255 };
	static const unsigned char zero_ip[4] = { 0, 0, 0, 0 };
	const struct netdev *nd = netdev_active();
	unsigned char my_mac[6], want_ip[4], current[4];
	unsigned char yiaddr[4], mask[4], gw[4], dns[4], server[4];
	unsigned int xid, start, deadline, tries;
	unsigned int lease = 0;

	if (!nd || !nd->get_mac)
		return -2;
	nd->get_mac(my_mac);
	net_get_ip(current);
	/* Try to get the address we would use anyway (static default or the
	 * previous lease): most routers honour option 50. */
	for (tries = 0; tries < 4; tries++)
		want_ip[tries] = current[tries];

	xid = (get_ticks_ms() << 8) ^ ((unsigned int)my_mac[3] << 16) ^
	      ((unsigned int)my_mac[4] << 8) ^ my_mac[5];
	start = get_ticks_ms();
	deadline = start + timeout_ms;

	netdev_acquire();
	io_putstr("dhcp: DISCOVER\n");

	/* DISCOVER -> OFFER */
	for (tries = 0; tries < 3; tries++) {
		unsigned int wait_until = get_ticks_ms() + 600;

		if ((int)(wait_until - deadline) > 0)
			wait_until = deadline;
		dhcp_build(DHCP_DISCOVER, my_mac, xid,
			   get_ticks_ms() - start, want_ip, 0);
		if (net_udp_send_from(zero_ip, bcast_mac, bcast_ip,
				      DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
				      msg, DHCP_MSG_LEN) != 0) {
			netdev_release();
			return -3;      /* TX failed (NIC present) */
		}
		if (dhcp_wait(nd, xid, my_mac, wait_until, DHCP_OFFER,
			      yiaddr, mask, gw, dns, &lease, server) == 0)
			break;
		if ((int)(get_ticks_ms() - deadline) >= 0)
			break;
	}
	if (tries == 3) {
		netdev_release();
		io_putstr("dhcp: no server, keeping ");
		put_ip(current);
		io_putchar('\n');
		return -1;
	}

	io_putstr("dhcp: offer ");
	put_ip(yiaddr);
	io_putstr(" from ");
	put_ip(server);
	io_putchar('\n');

	/* REQUEST -> ACK */
	for (tries = 0; tries < 3; tries++) {
		unsigned int wait_until = get_ticks_ms() + 600;
		int r;

		if ((int)(wait_until - deadline) > 0)
			wait_until = deadline;
		dhcp_build(DHCP_REQUEST, my_mac, xid,
			   get_ticks_ms() - start, yiaddr, server);
		if (net_udp_send_from(zero_ip, bcast_mac, bcast_ip,
				      DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
				      msg, DHCP_MSG_LEN) != 0) {
			netdev_release();
			return -3;      /* TX failed (NIC present) */
		}
		r = dhcp_wait(nd, xid, my_mac, wait_until, DHCP_ACK,
			      yiaddr, mask, gw, dns, &lease, server);
		if (r == 0)
			break;
		if (r == -2) {          /* NAK: start over */
			netdev_release();
			io_putstr("dhcp: NAK, retrying\n");
			return dhcp_run(timeout_ms);
		}
		if ((int)(get_ticks_ms() - deadline) >= 0)
			break;
	}
	if (tries == 3) {
		netdev_release();
		io_putstr("dhcp: no ACK, keeping ");
		put_ip(current);
		io_putchar('\n');
		return -1;
	}

	net_configure(yiaddr, mask, gw);
	if (dns[0] || dns[1] || dns[2] || dns[3])
		net_set_dns(dns);
	have_info = 1;
	{
		unsigned int i;

		for (i = 0; i < 4; i++)
			last_server[i] = server[i];
	}
	last_lease = lease;

	io_putstr("dhcp: ack ");
	put_ip(yiaddr);
	io_putstr("/");
	put_ip(mask);
	io_putstr(" gw ");
	put_ip(gw);
	if (dns[0] || dns[1] || dns[2] || dns[3]) {
		io_putstr(" dns ");
		put_ip(dns);
	}
	io_putstr(" lease ");
	put_dec(lease);
	io_putstr("s\n");

	netdev_release();
	return 0;
}

int dhcp_last_info(unsigned char server[4], unsigned int *lease_s)
{
	unsigned int i;

	if (!have_info)
		return 0;
	for (i = 0; i < 4; i++)
		server[i] = last_server[i];
	*lease_s = last_lease;
	return 1;
}
