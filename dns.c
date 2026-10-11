/* Minimal DNS resolver: an A-record query over UDP (RFC 1035).
 *
 * One question, up to four answers, three sends one second apart.  Name
 * compression is handled when skipping over the answer section; the reply's
 * checksum is not validated, matching the rest of the stack (net_udp_send()
 * sends with the UDP checksum disabled too).
 *
 * Polled like every other net command: the caller owns the NIC
 * (netdev_acquire) while dns_resolve() runs. */

#include "io.h"
#include "timer.h"
#include "netdev.h"
#include "net.h"
#include "dns.h"

#define DNS_PORT        53
#define DNS_MAX_ANSWERS 4
#define DNS_RETRY_MS    1000
#define DNS_RETRIES     3

/* Query buffer: 12-byte header + a 255-byte name + qtype/qclass. */
static unsigned char query_buf[12 + 260 + 4];

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

/* Encode a dotted name as DNS labels (no trailing dot needed); 0 on error. */
static int dns_encode_name(const char *name, unsigned char *out,
			   unsigned int max)
{
	const char *p = name;
	unsigned int o = 0;

	if (!*name)
		return -1;
	for (;;) {
		const char *q = p;
		unsigned int n = 0;

		while (*q && *q != '.') {
			q++;
			n++;
		}
		if (n == 0 || n > 63)
			return -1;
		/* length byte + label + a possible terminator must fit */
		if (o + 1 + n + 1 > max || o + 1 + n > 254)
			return -1;
		out[o++] = (unsigned char)n;
		while (p < q)
			out[o++] = (unsigned char)*p++;
		if (!*p)
			break;
		p++;                    /* skip the '.' */
		if (!*p)                /* trailing dot */
			break;
	}
	out[o++] = 0;
	return (int)o;
}

/* Skip one (possibly compressed) name; 0, or -1 when malformed. */
static int dns_skip_name(const unsigned char *p, unsigned int plen,
			 unsigned int *pos)
{
	while (*pos < plen) {
		unsigned char b = p[*pos];

		if (b == 0) {
			(*pos)++;
			return 0;
		}
		if ((b & 0xC0) == 0xC0) {
			if (*pos + 2 > plen)
				return -1;
			*pos += 2;       /* pointer: nothing else to skip */
			return 0;
		}
		if (b & 0xC0)
			return -1;
		if (*pos + 1 + b > plen)
			return -1;
		*pos += 1 + b;
	}
	return -1;
}

/* Check one frame as a reply to our query.  Returns:
 *   1 = at least one A record (ip and extras filled),
 *   0 = not our reply (keep waiting),
 *  -1 = our reply but a DNS-level failure (already reported). */
static int dns_parse(const unsigned char *frame, unsigned int len,
		     unsigned short id, const unsigned char *server,
		     unsigned char ip[4], unsigned char extra[DNS_MAX_ANSWERS][4],
		     unsigned int *count)
{
	unsigned char s_ip[4], d_ip[4];
	const unsigned char *p;
	unsigned int sport, dport, plen, pos, qd, an, i;

	if (!net_udp_parse(frame, len, s_ip, d_ip, &sport, &dport, &p, &plen))
		return 0;
	if (sport != DNS_PORT || !ip_eq(s_ip, server))
		return 0;
	if (plen < 12)
		return 0;
	if ((((unsigned int)p[0] << 8) | p[1]) != id)
		return 0;
	if (!(p[2] & 0x80))                      /* QR: must be a reply */
		return 0;
	if (p[2] & 0x02) {                       /* TC: truncated */
		io_putstr("dns: reply truncated (TC)\n");
		return -1;
	}
	switch (p[3] & 0x0F) {
	case 0:
		break;
	case 3:
		io_putstr("dns: no such name (NXDOMAIN)\n");
		return -1;
	case 2:
		io_putstr("dns: server failure (SERVFAIL)\n");
		return -1;
	default:
		io_putstr("dns: rcode ");
		put_dec(p[3] & 0x0F);
		io_putchar('\n');
		return -1;
	}

	qd = ((unsigned int)p[4] << 8) | p[5];
	an = ((unsigned int)p[6] << 8) | p[7];

	pos = 12;
	for (i = 0; i < qd; i++) {
		if (dns_skip_name(p, plen, &pos) != 0 || pos + 4 > plen)
			return 0;
		pos += 4;                        /* qtype + qclass */
	}

	*count = 0;
	for (i = 0; i < an; i++) {
		unsigned int type, class_, rdlen;

		if (dns_skip_name(p, plen, &pos) != 0 || pos + 10 > plen)
			return 0;
		type = ((unsigned int)p[pos] << 8) | p[pos + 1];
		class_ = ((unsigned int)p[pos + 2] << 8) | p[pos + 3];
		rdlen = ((unsigned int)p[pos + 8] << 8) | p[pos + 9];
		pos += 10;
		if (pos + rdlen > plen)
			return 0;
		if (type == 1 && class_ == 1 && rdlen == 4) {
			unsigned int k;

			if (*count == 0) {
				for (k = 0; k < 4; k++)
					ip[k] = p[pos + k];
			} else if (*count < DNS_MAX_ANSWERS) {
				for (k = 0; k < 4; k++)
					extra[*count - 1][k] = p[pos + k];
			}
			(*count)++;
		}
		pos += rdlen;
	}
	return *count > 0 ? 1 : 0;
}

int dns_resolve(const char *name, unsigned char ip[4])
{
	unsigned char server[4], hop[4], server_mac[6];
	unsigned char extra[DNS_MAX_ANSWERS][4];
	unsigned char *rx_buf = netdev_rx_scratch();
	const struct netdev *nd = netdev_active();
	unsigned short id;
	unsigned int sport, qlen, tries, count, i;
	int r;

	if (!nd) {
		io_putstr("dns: no NIC found\n");
		return -1;
	}

	net_get_dns(server);
	if (!(server[0] || server[1] || server[2] || server[3])) {
		/* Home routers usually proxy DNS for their clients. */
		net_get_gateway(server);
		if (!(server[0] || server[1] || server[2] || server[3])) {
			io_putstr("dns: no resolver (run netdns <a.b.c.d>)\n");
			return -1;
		}
	}

	for (i = 0; i < 12; i++)
		query_buf[i] = 0;
	id = (unsigned short)(get_ticks_ms() ^
			      ((unsigned int)(unsigned char)name[0] << 8) ^
			      0x4E42);
	query_buf[0] = (unsigned char)(id >> 8);
	query_buf[1] = (unsigned char)id;
	query_buf[2] = 0x01;                  /* RD=1: recursion desired */
	query_buf[5] = 1;                     /* QDCOUNT=1 */
	qlen = 12;
	{
		int n = dns_encode_name(name, query_buf + 12,
					sizeof(query_buf) - 12 - 4);

		if (n <= 0) {
			io_putstr("dns: bad name\n");
			return -1;
		}
		qlen += (unsigned int)n;
	}
	query_buf[qlen++] = 0;                /* QTYPE A */
	query_buf[qlen++] = 1;
	query_buf[qlen++] = 0;                /* QCLASS IN */
	query_buf[qlen++] = 1;

	sport = 45000 + (id & 0x0FFF);

	net_next_hop(server, hop);
	if (net_arp_resolve(hop, server_mac) != 0) {
		io_putstr("dns: no ARP reply from ");
		put_ip(hop);
		io_putchar('\n');
		return -1;
	}

	for (tries = 0; tries < DNS_RETRIES; tries++) {
		unsigned int deadline = get_ticks_ms() + DNS_RETRY_MS;

		if (net_udp_send(server_mac, server, sport, DNS_PORT,
				 query_buf, qlen) != 0) {
			io_putstr("dns: send failed\n");
			return -1;
		}

		while ((int)(get_ticks_ms() - deadline) < 0) {
			int len = nd->rx(rx_buf, NETDEV_FRAME_MAX);

			if (len < 0)
				continue;
			r = dns_parse(rx_buf, (unsigned int)len, id, server,
				      ip, extra, &count);
			if (r < 0)
				return -1;
			if (r == 0)
				continue;
			io_putstr("dns: ");
			io_putstr(name);
			io_putstr(" -> ");
			put_ip(ip);
			for (i = 1; i < count && i < DNS_MAX_ANSWERS; i++) {
				io_putstr(", ");
				put_ip(extra[i - 1]);
			}
			io_putchar('\n');
			return 0;
		}
	}

	io_putstr("dns: no reply from ");
	put_ip(server);
	io_putchar('\n');
	return -1;
}

/* ---- commands ---- */

void program_netdns_main(const char *arg)
{
	unsigned char dns[4], gw[4];

	if (arg[0] == '\0') {
		net_get_dns(dns);
		if (dns[0] || dns[1] || dns[2] || dns[3]) {
			io_putstr("dns=");
			put_ip(dns);
			io_putchar('\n');
			return;
		}
		net_get_gateway(gw);
		io_putstr("dns: unset");
		if (gw[0] || gw[1] || gw[2] || gw[3]) {
			io_putstr(" (fallback: gateway ");
			put_ip(gw);
			io_putchar(')');
		}
		io_putchar('\n');
		return;
	}

	if (net_parse_ip(arg, dns) != 0) {
		io_putstr("netdns: usage netdns [a.b.c.d]\n");
		return;
	}

	net_set_dns(dns);
	io_putstr("dns set to ");
	put_ip(dns);
	io_putchar('\n');
}

void program_lookup_main(const char *arg)
{
	unsigned char ip[4];

	if (arg[0] == '\0') {
		io_putstr("lookup: usage lookup <name>\n");
		return;
	}

	netdev_acquire();
	dns_resolve(arg, ip);
	netdev_release();
}
