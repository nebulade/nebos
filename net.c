/* Minimal network stack: ARP + ICMP echo (ping) over the NE2000 driver.
   Freestanding, no malloc; fixed-size buffers and a single configured IP. */

#include "io.h"
#include "ne2000.h"
#include "net.h"

static unsigned char my_ip[4] = { 192, 168, 0, 99 };

/* ---- small print helpers (framebuffer console) ---- */

static void print_hex2(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
}

static void print_hex4(unsigned int v)
{
	print_hex2((v >> 8) & 0xFF);
	print_hex2(v & 0xFF);
}

static void print_dec(unsigned int v)
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

static void print_mac(const unsigned char *m)
{
	unsigned int i;
	for (i = 0; i < 6; i++) {
		if (i)
			io_putchar(':');
		print_hex2(m[i]);
	}
}

static void print_ip(const unsigned char *p)
{
	unsigned int i;
	for (i = 0; i < 4; i++) {
		if (i)
			io_putchar('.');
		print_dec(p[i]);
	}
}

static int ip_eq(const unsigned char *a, const unsigned char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Internet checksum (16-bit one's complement, network byte order). */
static unsigned short checksum(const void *data, unsigned int len)
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

/* ---- IP address config ---- */

void net_set_ip(const unsigned char ip[4])
{
	unsigned int i;
	for (i = 0; i < 4; i++)
		my_ip[i] = ip[i];
}

void net_get_ip(unsigned char ip[4])
{
	unsigned int i;
	for (i = 0; i < 4; i++)
		ip[i] = my_ip[i];
}

int net_parse_ip(const char *s, unsigned char ip[4])
{
	unsigned int i, val = 0, nparts = 0;
	int have_digit = 0;

	for (i = 0; ; i++) {
		char c = s[i];
		if (c >= '0' && c <= '9') {
			val = val * 10 + (unsigned int)(c - '0');
			if (val > 255)
				return -1;
			have_digit = 1;
		} else if (c == '.' || c == '\0') {
			if (!have_digit || nparts >= 4)
				return -1;
			ip[nparts++] = (unsigned char)val;
			val = 0;
			have_digit = 0;
			if (c == '\0')
				break;
		} else {
			return -1;
		}
	}
	return (nparts == 4) ? 0 : -1;
}

void net_print_ip(void)
{
	print_ip(my_ip);
}

/* ---- ethertype naming ---- */

const char *net_ethertype_name(unsigned short et)
{
	switch (et) {
	case ET_IPV4: return "IPv4";
	case ET_ARP:  return "ARP";
	case ET_IPV6: return "IPv6";
	default:      return 0;
	}
}

/* ---- frame decoding ---- */

void net_decode_frame(const unsigned char *f, unsigned int len)
{
	unsigned short et;
	const char *name;

	if (len < 14) {
		io_putstr("(short frame len=");
		print_dec(len);
		io_putstr(")");
		return;
	}

	et = ((unsigned short)f[12] << 8) | f[13];

	print_mac(f + 6);
	io_putstr(" -> ");
	print_mac(f);
	io_putstr("  ");
	name = net_ethertype_name(et);
	if (name)
		io_putstr(name);
	else {
		io_putstr("0x");
		print_hex4(et);
	}
	io_putstr(" len=");
	print_dec(len);

	if (et == ET_ARP && len >= 14 + 28) {
		const unsigned char *a = f + 14;
		unsigned short op = ((unsigned short)a[6] << 8) | a[7];
		if (op == 1) {
			io_putstr("  who-has ");
			print_ip(a + 24);
			io_putstr(" tell ");
			print_ip(a + 14);
		} else if (op == 2) {
			io_putstr("  ");
			print_ip(a + 14);
			io_putstr(" is-at ");
			print_mac(a + 8);
		}
	} else if (et == ET_IPV4 && len >= 14 + 20) {
		const unsigned char *ip = f + 14;
		unsigned int ihl = (unsigned int)(ip[0] & 0x0F) * 4;
		unsigned char proto = ip[9];
		io_putstr("  ");
		print_ip(ip + 12);
		io_putstr(" -> ");
		print_ip(ip + 16);
		io_putstr(" proto=");
		print_dec(proto);
		if (proto == IPPROTO_ICMP && ihl >= 20 && len >= 14 + ihl + 8) {
			const unsigned char *ic = ip + ihl;
			io_putstr("  ICMP type=");
			print_dec(ic[0]);
			io_putstr(" code=");
			print_dec(ic[1]);
		}
	}
}

/* ---- reply generation ---- */

int net_build_reply(const unsigned char *frame, unsigned int len,
		    unsigned char *out, unsigned int maxout)
{
	unsigned char mac[6];
	unsigned short et;
	unsigned int i;

	if (len < 14)
		return 0;
	et = ((unsigned short)frame[12] << 8) | frame[13];

	ne2000_get_mac(mac);

	/* ARP request for our IP -> ARP reply. */
	if (et == ET_ARP && len >= 14 + 28) {
		const unsigned char *a = frame + 14;
		unsigned short op = ((unsigned short)a[6] << 8) | a[7];

		if (op != 1)
			return 0;
		if ((((unsigned short)a[0] << 8) | a[1]) != 1)   /* htype Ethernet */
			return 0;
		if ((((unsigned short)a[2] << 8) | a[3]) != ET_IPV4)
			return 0;
		if (a[4] != 6 || a[5] != 4)
			return 0;
		if (!ip_eq(a + 24, my_ip))                       /* target != us */
			return 0;

		for (i = 0; i < 6; i++)
			out[i] = frame[6 + i];            /* dst = requester */
		for (i = 0; i < 6; i++)
			out[6 + i] = mac[i];              /* src = us */
		out[12] = 0x08; out[13] = 0x06;       /* ethertype ARP */

		{
			unsigned char *r = out + 14;
			r[0] = 0x00; r[1] = 0x01;         /* htype Ethernet */
			r[2] = 0x08; r[3] = 0x00;         /* ptype IPv4 */
			r[4] = 6; r[5] = 4;
			r[6] = 0x00; r[7] = 0x02;         /* opcode reply */
			for (i = 0; i < 6; i++)
				r[8 + i] = mac[i];        /* sha */
			for (i = 0; i < 4; i++)
				r[14 + i] = my_ip[i];     /* spa */
			for (i = 0; i < 6; i++)
				r[18 + i] = a[8 + i];     /* tha = requester MAC */
			for (i = 0; i < 4; i++)
				r[24 + i] = a[14 + i];    /* tpa = requester IP */
		}
		return 42;
	}

	/* ICMP echo request for our IP -> ICMP echo reply. */
	if (et == ET_IPV4 && len >= 14 + 20) {
		const unsigned char *ip = frame + 14;
		unsigned int ihl, icmp_len, reply_len;
		unsigned short ip_len;
		const unsigned char *ic;
		unsigned char *rip, *ric;

		if ((ip[0] >> 4) != 4)
			return 0;
		ihl = (unsigned int)(ip[0] & 0x0F) * 4;
		if (ihl < 20)
			return 0;
		if (ip[9] != IPPROTO_ICMP)
			return 0;
		if (!ip_eq(ip + 16, my_ip))
			return 0;

		ip_len = ((unsigned short)ip[2] << 8) | ip[3];
		if (ip_len < ihl || ip_len > len - 14)
			return 0;
		if (len < 14 + ihl + 8)
			return 0;

		ic = ip + ihl;
		if (ic[0] != ICMP_ECHO_REQUEST)
			return 0;

		icmp_len = ip_len - ihl;
		reply_len = 14 + ip_len;
		if (reply_len > maxout)
			return 0;

		/* Ethernet header. */
		for (i = 0; i < 6; i++)
			out[i] = frame[6 + i];
		for (i = 0; i < 6; i++)
			out[6 + i] = mac[i];
		out[12] = 0x08; out[13] = 0x00;       /* ethertype IPv4 */

		/* IPv4 header (IHL 5, no options). */
		rip = out + 14;
		rip[0] = 0x45;
		rip[1] = 0x00;
		rip[2] = ip[2]; rip[3] = ip[3];       /* total length (same) */
		rip[4] = ip[4]; rip[5] = ip[5];       /* id */
		rip[6] = 0x00; rip[7] = 0x00;         /* flags/frag */
		rip[8] = 64;                          /* TTL */
		rip[9] = IPPROTO_ICMP;
		rip[10] = 0; rip[11] = 0;             /* checksum (computed below) */
		for (i = 0; i < 4; i++)
			rip[12 + i] = my_ip[i];         /* src = us */
		for (i = 0; i < 4; i++)
			rip[16 + i] = ip[12 + i];       /* dst = requester */

		/* ICMP: copy the whole request, then fix type/code/checksum. */
		ric = rip + ihl;
		for (i = 0; i < icmp_len; i++)
			ric[i] = ic[i];
		ric[0] = ICMP_ECHO_REPLY;
		ric[1] = 0;
		ric[2] = 0; ric[3] = 0;

		{
			unsigned short cs = checksum(rip, ihl);
			rip[10] = (unsigned char)(cs >> 8);
			rip[11] = (unsigned char)(cs & 0xFF);
		}
		{
			unsigned short cs = checksum(ric, icmp_len);
			ric[2] = (unsigned char)(cs >> 8);
			ric[3] = (unsigned char)(cs & 0xFF);
		}

		return (int)reply_len;
	}

	return 0;
}
