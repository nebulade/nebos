/* ping: ring-3 command.  Active ARP + ICMP echo (via the raw frame syscalls). */

#include "ucmd_lib.h"

#define ET_IPV4 0x0800
#define ET_ARP  0x0806
#define IPPROTO_ICMP 1
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define NET_FRAME_MAX 1518
#define PING_COUNT  4
#define PING_ID     0x4E42   /* "NB" */
#define PING_WAIT_MS 1000

static unsigned char rx_buf[NET_FRAME_MAX];
static unsigned int ping_sent_ms[PING_COUNT];

static unsigned short net_checksum(const void *data, unsigned int len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int sum = 0;

	while (len > 1) {
		sum += ((unsigned int)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (unsigned int)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (unsigned short)(~sum);
}

static int ip_eq(const unsigned char *a, const unsigned char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

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
	ic[4] = (unsigned char)(PING_ID >> 8);
	ic[5] = (unsigned char)(PING_ID & 0xFF);
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
	if (id != PING_ID)
		return 0;
	*seq = ((unsigned short)ic[6] << 8) | ic[7];
	if (*seq < 1 || *seq > PING_COUNT)
		return 0;
	return 1;
}

/* Resolve target_ip's MAC by ARP (3 tries, 1 s each).
 * Returns 0 = resolved, -1 = no reply, -2 = transmit failed.
 * Prints the frames seen while waiting (capped) so a silent peer, a filtered
 * reply and a reply we refuse to parse can be told apart on screen. */
static int ping_arp(const unsigned char *my_mac, const unsigned char *my_ip,
		    const unsigned char *target_ip, unsigned char *target_mac)
{
	unsigned char frame[64];
	unsigned int tries, seen = 0;

	for (tries = 0; tries < 3; tries++) {
		unsigned int deadline = u_ticks_ms() + PING_WAIT_MS;
		unsigned int flen = build_arp_request(my_mac, my_ip, target_ip, frame);

		if (u_syscall2(SYS_NET_TX, (int)frame, flen) != 0)
			return -2;
		u_puts("ping: ARP request sent (try ");
		u_putdec(tries + 1);
		u_puts("/3)\n");
		while ((int)(u_ticks_ms() - deadline) < 0) {
			int len = u_syscall2(SYS_NET_RX, (int)rx_buf, NET_FRAME_MAX);

			if (len < 0)
				continue;
			if (seen < 8) {
				u_puts("ping: rx ");
				u_syscall2(SYS_NET_DECODE, (int)rx_buf,
					   (unsigned int)len);
				u_putc('\n');
				seen++;
			}
			if (parse_arp_reply(rx_buf, (unsigned int)len,
					    target_ip, target_mac))
				return 0;
		}
	}
	u_puts("ping: saw ");
	u_putdec(seen);
	u_puts(" frame(s) while waiting\n");
	return -1;
}

void _start(const char *arg)
{
	unsigned char target[4], my_ip[4], my_mac[6], target_mac[6];
	unsigned char mask[4], gw[4], hop[4];
	unsigned int i, replies = 0;

	if (!arg || !arg[0]) {
		u_puts("ping: usage ping <host|a.b.c.d>\n");
		u_exit(1);
	}

	if (u_syscall0(SYS_NET_ACQUIRE) != 0) {
		u_puts("ping: no NIC found\n");
		u_exit(1);
	}

	/* Dotted IP, or a name to resolve first. */
	if (u_parse_ip(arg, target) != 0) {
		if (u_syscall2(SYS_DNS_RESOLVE, (int)arg, (int)target) != 0) {
			u_syscall0(SYS_NET_RELEASE);
			u_exit(1);
		}
	}

	u_syscall1(SYS_NET_MAC, (int)my_mac);
	u_syscall1(SYS_NET_GET_IP, (int)my_ip);

	/* With a mask and gateway known, off-subnet targets go via the router;
	 * we can only ARP for the next hop. */
	u_syscall1(SYS_NET_GET_MASK, (int)mask);
	u_syscall1(SYS_NET_GET_GW, (int)gw);
	{
		unsigned int j, off = 0;

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
			u_puts("ping: ");
			u_put_ip(target);
			u_puts(" via ");
			u_put_ip(hop);
			u_putc('\n');
		}

		{
			int r = ping_arp(my_mac, my_ip, hop, target_mac);

			if (r == -2) {
				u_puts("ping: transmit failed (see netregs)\n");
				u_syscall0(SYS_NET_RELEASE);
				u_exit(1);
			}
			if (r != 0) {
				u_puts("ping: no ARP reply from ");
				u_put_ip(hop);
				u_putc('\n');
				u_syscall0(SYS_NET_RELEASE);
				u_exit(1);
			}
		}
	}
	u_puts("ping: ");
	u_put_ip(target);
	u_puts(" is-at ");
	u_put_mac(target_mac);
	u_putc('\n');

	for (i = 0; i < PING_COUNT; i++) {
		unsigned char frame[64];
		unsigned int flen, deadline;
		int got = 0;

		flen = build_icmp_echo(my_mac, target_mac, my_ip, target,
				       (unsigned short)(i + 1), frame);
		ping_sent_ms[i] = u_ticks_ms();
		if (u_syscall2(SYS_NET_TX, (int)frame, flen) != 0) {
			u_puts("ping: transmit failed\n");
			u_syscall0(SYS_NET_RELEASE);
			u_exit(1);
		}

		deadline = u_ticks_ms() + PING_WAIT_MS;
		while ((int)(u_ticks_ms() - deadline) < 0 && !got) {
			unsigned short seq;
			int len = u_syscall2(SYS_NET_RX, (int)rx_buf, NET_FRAME_MAX);
			unsigned int rtt;

			if (len < 0)
				continue;
			if (!parse_icmp_reply(rx_buf, (unsigned int)len,
					      my_ip, target, &seq))
				continue;
			rtt = u_ticks_ms() - ping_sent_ms[seq - 1];
			u_puts("reply: seq=");
			u_putdec(seq);
			u_puts(" rtt=");
			if (rtt == 0)
				u_puts("<1");
			else
				u_putdec(rtt);
			u_puts("ms\n");
			got = 1;
			replies++;
		}
		if (!got) {
			u_puts("ping: timeout, seq=");
			u_putdec(i + 1);
			u_putc('\n');
		}
	}

	u_puts("ping: ");
	u_putdec(replies);
	u_puts("/");
	u_putdec(PING_COUNT);
	u_puts(" replies from ");
	u_put_ip(target);
	u_putc('\n');
	u_syscall0(SYS_NET_RELEASE);
	u_exit(replies ? 0 : 1);
}
