/* Minimal SNTP client (RFC 4330).
 *
 * One 48-byte client request to UDP port 123, three tries one second apart.
 * The server is resolved through DNS (a dotted address skips it) and reached
 * through the shared ARP/routing helpers.  The reply's origin timestamp must
 * match the request's transmit timestamp, and its transmit timestamp is
 * converted from NTP seconds (1900 epoch) to UTC `struct sys_datetime`.
 *
 * Like DNS and DHCP the polled caller owns the NIC; sys_ntp_sync() acquires
 * it for ring-3 callers.  No NTP authentication/delay estimation is done:
 * the RTC only has one-second resolution. */

#include "ntp.h"
#include "dns.h"
#include "net.h"
#include "netdev.h"
#include "timer.h"

#define NTP_PORT        123
#define NTP_PACKET_LEN  48
#define NTP_RETRY_MS    1000
#define NTP_RETRIES     3
#define NTP_DEFAULT_TIMEOUT_MS (NTP_RETRIES * NTP_RETRY_MS)

/* 1900-01-01 -> 1970-01-01, and the Unix time of the NTP 32-bit wrap. */
#define NTP_UNIX_EPOCH  2208988800u
#define NTP_ERA1_BASE   2085978496u

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

static int ip_eq(const unsigned char *a, const unsigned char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* NTP seconds/fraction -> Unix seconds.  The 32-bit NTP seconds wrap in
 * 2036; values below the 1968 boundary belong to era 1 then.  The RTC range
 * is 1970-2069, so the 1968-1970 era-0 window is simply rejected. */
static int ntp_to_unix(unsigned int secs, unsigned int frac,
		       unsigned int *out)
{
	unsigned int t;

	if (secs >= NTP_UNIX_EPOCH)
		t = secs - NTP_UNIX_EPOCH;              /* era 0: 1970..2036 */
	else if (secs < NTP_ERA1_BASE)
		t = secs + NTP_ERA1_BASE;               /* era 1: 2036..2106 */
	else
		return -1;                              /* 1968..1970 */
	if (frac >= 0x80000000u)                        /* round to nearest */
		t++;
	*out = t;
	return 0;
}

/* Howard Hinnant's civil-from-days algorithm, all unsigned 32-bit. */
static void unix_to_datetime(unsigned int t, struct sys_datetime *dt)
{
	unsigned int days = t / 86400u;
	unsigned int rem = t % 86400u;
	unsigned int z = days + 719468u;
	unsigned int era = z / 146097u;
	unsigned int doe = z - era * 146097u;
	unsigned int yoe = (doe - doe / 1460u + doe / 36524u -
			    doe / 146096u) / 365u;
	unsigned int y = yoe + era * 400u;
	unsigned int doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	unsigned int mp = (5u * doy + 2u) / 153u;
	unsigned int d = doy - (153u * mp + 2u) / 5u + 1u;
	unsigned int m = mp < 10u ? mp + 3u : mp - 9u;

	if (m <= 2u)
		y++;
	dt->year = y;
	dt->month = m;
	dt->day = d;
	dt->hour = rem / 3600u;
	dt->minute = (rem % 3600u) / 60u;
	dt->second = rem % 60u;
	dt->wday = (days + 4u) % 7u;   /* 1970-01-01 was a Thursday */
}

/* 1 = a valid reply and *out filled, 0 = not ours, <0 = ours but unusable. */
static int ntp_parse(const unsigned char *frame, unsigned int len,
		     const unsigned char *server_ip, unsigned int server_port,
		     unsigned int nonce, unsigned int nfrac,
		     struct sys_datetime *out)
{
	unsigned char s_ip[4], d_ip[4];
	const unsigned char *p;
	unsigned int sport, dport, plen, secs, frac, unix_sec;
	int mode;

	if (!net_udp_parse(frame, len, s_ip, d_ip, &sport, &dport, &p, &plen))
		return 0;
	if (sport != NTP_PORT || dport != server_port ||
	    !ip_eq(s_ip, server_ip))
		return 0;
	if (plen < NTP_PACKET_LEN)
		return 0;

	mode = p[0] & 0x07;
	if (mode != 4 && mode != 5)          /* server or broadcast reply */
		return 0;
	if ((p[0] >> 6) == 3)                /* LI: server unsynchronised */
		return -1;
	if (p[1] == 0 || p[1] > 15)          /* kiss-o'-death / invalid */
		return -1;
	if (rd32be(p + 24) != nonce || rd32be(p + 28) != nfrac)
		return 0;                    /* not a reply to this request */

	secs = rd32be(p + 40);
	frac = rd32be(p + 44);
	if (!(secs | frac))                  /* no transmit timestamp */
		return -1;
	if (ntp_to_unix(secs, frac, &unix_sec) != 0)
		return -1;
	unix_to_datetime(unix_sec, out);
	if (out->year < 1970 || out->year > 2069)
		return -1;
	return 1;
}

int ntp_sync(const char *server, unsigned int timeout_ms,
	     struct sys_datetime *out)
{
	const struct netdev *nd = netdev_active();
	unsigned char server_ip[4], hop[4], mac[6];
	unsigned char *rxbuf;
	unsigned char pkt[NTP_PACKET_LEN];
	unsigned int nonce, nfrac, sport, tries, start, deadline, i;
	char name[64];
	unsigned int n = 0, e = 0;
	int r;

	if (!nd || !nd->rx || !nd->tx || !nd->get_mac)
		return SYS_NTP_ENONIC;
	if (!out)
		return SYS_NTP_EREPLY;

	/* The text field may have accidental surrounding spaces. */
	if (server) {
		while (server[n] == ' ' || server[n] == '\t')
			n++;
		while (server[n] && e + 1 < sizeof(name))
			name[e++] = server[n++];
		while (e && (name[e - 1] == ' ' || name[e - 1] == '\t'))
			e--;
	}
	name[e] = '\0';
	if (!name[0]) {
		static const char fallback[] = NTP_DEFAULT_SERVER;

		for (i = 0; fallback[i] && i + 1 < sizeof(name); i++)
			name[i] = fallback[i];
		name[i] = '\0';
	}

	if (net_parse_ip(name, server_ip) != 0) {
		if (dns_resolve(name, server_ip) != 0)
			return SYS_NTP_ENAME;
	}
	net_next_hop(server_ip, hop);
	r = net_arp_resolve(hop, mac);
	if (r == -2)
		return SYS_NTP_ETX;
	if (r != 0)
		return SYS_NTP_EARP;

	nonce = get_ticks_ms() ^ 0x4E545000u;    /* "NT" */
	if (!nonce)
		nonce = 1;
	nfrac = 0x4E420000u;                     /* "NB" */
	for (i = 0; i < NTP_PACKET_LEN; i++)
		pkt[i] = 0;
	pkt[0] = 0x23;                           /* LI 0, VN 4, mode 3 client */
	pkt[2] = 6;                              /* poll exponent: 64 s */
	pkt[3] = 0xEC;                           /* precision: -20 */
	wr32be(pkt + 40, nonce);
	wr32be(pkt + 44, nfrac);

	/* Ephemeral source port; the reply must come back to it. */
	sport = 49152u + (nonce & 0x3FFFu);
	if (timeout_ms == 0)
		timeout_ms = NTP_DEFAULT_TIMEOUT_MS;
	if (timeout_ms > 15000)
		timeout_ms = 15000;

	rxbuf = netdev_rx_scratch();
	start = get_ticks_ms();
	for (tries = 0; tries < NTP_RETRIES; tries++) {
		unsigned int now = get_ticks_ms();

		if ((int)(now - (start + timeout_ms)) >= 0)
			break;
		deadline = now + NTP_RETRY_MS;
		if ((int)(deadline - (start + timeout_ms)) > 0)
			deadline = start + timeout_ms;

		if (net_udp_send(mac, server_ip, sport, NTP_PORT,
				 pkt, sizeof(pkt)) != 0)
			return SYS_NTP_ETX;

		while ((int)(get_ticks_ms() - deadline) < 0) {
			int len = nd->rx(rxbuf, NETDEV_FRAME_MAX);

			if (len < 0)
				continue;
			r = ntp_parse(rxbuf, (unsigned int)len, server_ip,
				      sport, nonce, nfrac, out);
			if (r > 0)
				return SYS_NTP_OK;
			if (r < 0)
				return SYS_NTP_EREPLY;
		}
	}
	return SYS_NTP_ETIMEOUT;
}
