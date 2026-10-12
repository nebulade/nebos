/* Minimal TCP client: see tcp.h. */

#include "io.h"
#include "timer.h"
#include "netdev.h"
#include "net.h"
#include "tcp.h"

/* TCP flags. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_MSS_OURS  1460
#define TCP_WINDOW    4096
#define TCP_RETRY_MS  1000
#define TCP_RETRIES   3

/* Frame buffers are file scope: a 4 KB process stack cannot hold them.  The
 * receive side uses the shared frame scratch (netdev_rx_scratch()). */
static unsigned char tx_frame[14 + 20 + 20 + TCP_MSS_OURS];

/* Receive queue: in-order payload accepted from the peer but not yet handed
 * to the application.  Data can arrive while the app is inside tcp_send()
 * (a TLS server sends session tickets right after the handshake), so it must
 * be buffered, never ACKed and dropped.  A segment that does not fit is left
 * unacknowledged and the peer retransmits it. */
#define TCP_RXQ_SIZE 8192
static unsigned char rxq[TCP_RXQ_SIZE];
static unsigned int rxq_head, rxq_tail;

static unsigned int rxq_len(void)
{
	return (rxq_head - rxq_tail) & (TCP_RXQ_SIZE - 1);
}

static unsigned int rxq_space(void)
{
	return TCP_RXQ_SIZE - 1 - rxq_len();
}

static void rxq_put(const unsigned char *p, unsigned int n)
{
	while (n--) {
		rxq[rxq_head] = *p++;
		rxq_head = (rxq_head + 1) & (TCP_RXQ_SIZE - 1);
	}
}

static void rxq_get(unsigned char *p, unsigned int n)
{
	while (n--) {
		*p++ = rxq[rxq_tail];
		rxq_tail = (rxq_tail + 1) & (TCP_RXQ_SIZE - 1);
	}
}

static unsigned int rd16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | p[1];
}

static void wr16(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static unsigned int rd32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static void wr32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/* Build and transmit one segment.  `seq` is the sequence of the first byte
 * (for SYN/FIN that is the flag's own sequence).  Returns 0 / -1 (tx). */
static int tcp_send_segment(struct tcp_conn *c, unsigned char flags,
			    unsigned int seq, unsigned int ack,
			    const unsigned char *payload, unsigned int len,
			    int with_mss)
{
	const struct netdev *nd = netdev_active();
	unsigned char my_mac[6], my_ip[4];
	unsigned char *ip = tx_frame + 14;
	unsigned char *tcp = ip + 20;
	unsigned short cs;
	unsigned int total, optlen = 0, i;

	if (!nd)
		return -1;
	nd->get_mac(my_mac);
	net_get_ip(my_ip);

	/* Ethernet. */
	for (i = 0; i < 6; i++)
		tx_frame[i] = c->dst_mac[i];
	for (i = 0; i < 6; i++)
		tx_frame[6 + i] = my_mac[i];
	tx_frame[12] = 0x08;
	tx_frame[13] = 0x00;

	/* IPv4 header (IHL 5, no options). */
	total = 20 + 20 + (with_mss ? 4 : 0) + len;
	for (i = 0; i < 20; i++)
		ip[i] = 0;
	ip[0] = 0x45;
	wr16(ip + 2, total);
	ip[8] = 64;                       /* TTL */
	ip[9] = IPPROTO_TCP;
	for (i = 0; i < 4; i++)
		ip[12 + i] = my_ip[i];
	for (i = 0; i < 4; i++)
		ip[16 + i] = c->dst_ip[i];
	cs = net_checksum(ip, 20);
	wr16(ip + 10, cs);

	/* TCP header (+ MSS option on SYN). */
	if (with_mss) {
		tcp[20] = 2;              /* kind: MSS */
		tcp[21] = 4;              /* length */
		wr16(tcp + 22, TCP_MSS_OURS);
		optlen = 4;
	}
	wr16(tcp + 0, c->sport);
	wr16(tcp + 2, c->dport);
	wr32(tcp + 4, seq);
	wr32(tcp + 8, ack);
	tcp[12] = (unsigned char)((5 + optlen / 4) << 4);
	tcp[13] = flags;
	wr16(tcp + 14, TCP_WINDOW);
	wr16(tcp + 16, 0);                /* checksum */
	wr16(tcp + 18, 0);                /* urgent pointer */
	for (i = 0; i < len; i++)
		tcp[20 + optlen + i] = payload[i];

	cs = net_checksum_tcp(my_ip, c->dst_ip, tcp, 20 + optlen + len);
	wr16(tcp + 16, cs);

	return nd->tx(tx_frame, 14 + total);
}

/* One received segment. */
struct tcp_seg {
	unsigned int  seq, ack;
	unsigned char flags;
	const unsigned char *data;
	unsigned int  len;
	const unsigned char *tcp;   /* for option parsing */
	unsigned int  doff;
};

/* Parse one frame as a TCP segment from the peer.  Returns 1 on match.
 * Incoming checksums are not validated (same as the UDP path). */
static int tcp_parse(const unsigned char *f, unsigned int flen,
		     const unsigned char *peer_ip,
		     unsigned int peer_port, unsigned int local_port,
		     struct tcp_seg *s)
{
	const unsigned char *ip = f + 14;
	const unsigned char *tcp;
	unsigned char my_ip[4];
	unsigned int i, ihl, ip_len, doff;

	if (flen < 14 + 20)
		return 0;
	if ((((unsigned int)f[12] << 8) | f[13]) != ET_IPV4)
		return 0;
	if ((ip[0] >> 4) != 4)
		return 0;
	ihl = (unsigned int)(ip[0] & 0x0F) * 4;
	if (ihl < 20 || ip[9] != IPPROTO_TCP)
		return 0;
	/* Fragments are not reassembled: MF or a nonzero offset. */
	if ((ip[6] & 0x3F) != 0 || ip[7] != 0)
		return 0;
	ip_len = rd16(ip + 2);
	if (ip_len < ihl + 20 || ip_len > flen - 14)
		return 0;

	net_get_ip(my_ip);
	for (i = 0; i < 4; i++) {
		if (ip[12 + i] != peer_ip[i])
			return 0;
		if (ip[16 + i] != my_ip[i])
			return 0;
	}

	tcp = ip + ihl;
	if (rd16(tcp + 0) != peer_port || rd16(tcp + 2) != local_port)
		return 0;
	doff = (unsigned int)(tcp[12] >> 4) * 4;
	if (doff < 20 || ihl + doff > ip_len)
		return 0;

	s->seq = rd32(tcp + 4);
	s->ack = rd32(tcp + 8);
	s->flags = tcp[13];
	s->data = tcp + doff;
	s->len = ip_len - ihl - doff;
	s->tcp = tcp;
	s->doff = doff;
	return 1;
}

/* Parse the peer's MSS option (default stays when absent); 1 if present. */
static int tcp_parse_mss(const unsigned char *tcp, unsigned int doff,
			 unsigned int *mss)
{
	unsigned int o = 20;

	while (o < doff) {
		unsigned char kind = tcp[o];

		if (kind == 0)                    /* end of options */
			return 0;
		if (kind == 1) {                  /* NOP */
			o++;
			continue;
		}
		if (o + 1 >= doff)
			return 0;
		if (tcp[o + 1] < 2 || o + tcp[o + 1] > doff)
			return 0;
		if (kind == 2 && tcp[o + 1] == 4) {
			*mss = rd16(tcp + o + 2);
			return 1;
		}
		o += tcp[o + 1];
	}
	return 0;
}

/* Take an incoming segment's payload/FIN into the receive queue.  Returns
 * -2 on RST, 0 otherwise.  Duplicate/out-of-order data is re-ACKed to resync;
 * a segment that does not fit the queue is not ACKed at all. */
static int tcp_rx_data(struct tcp_conn *c, const struct tcp_seg *s)
{
	if (s->flags & TCP_RST)
		return -2;

	if (s->len) {
		if (s->seq == c->rcv_nxt && s->len <= rxq_space()) {
			rxq_put(s->data, s->len);
			c->rcv_nxt += s->len;
			tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
					 0, 0, 0);
		} else if (s->seq != c->rcv_nxt) {
			tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
					 0, 0, 0);
		}
	}
	if ((s->flags & TCP_FIN) && !c->peer_fin &&
	    s->seq + s->len == c->rcv_nxt) {
		c->peer_fin = 1;
		c->rcv_nxt++;
		tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
				 0, 0, 0);
	}
	return 0;
}

int tcp_connect(const unsigned char *dst_mac, const unsigned char *dst_ip,
		unsigned int dport, struct tcp_conn *c)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rxbuf = netdev_rx_scratch();
	unsigned int tries, i;

	if (!nd || !net_is_configured())
		return -3;

	for (i = 0; i < 6; i++)
		c->dst_mac[i] = dst_mac[i];
	for (i = 0; i < 4; i++)
		c->dst_ip[i] = dst_ip[i];
	c->dport = dport;
	c->sport = 0xC000u | (get_ticks_ms() & 0x3FFFu);
	c->snd_una = c->snd_nxt = (get_ticks_ms() << 16) ^ 0x5EED0000u;
	c->rcv_nxt = 0;
	c->peer_mss = 536;
	c->peer_fin = 0;
	rxq_head = rxq_tail = 0;

	for (tries = 0; tries < TCP_RETRIES; tries++) {
		unsigned int deadline = get_ticks_ms() + TCP_RETRY_MS;

		if (tcp_send_segment(c, TCP_SYN, c->snd_nxt, 0, 0, 0, 1) != 0)
			return -3;

		while ((int)(get_ticks_ms() - deadline) < 0) {
			struct tcp_seg s;
			int len = nd->rx(rxbuf, NETDEV_FRAME_MAX);

			if (len < 0)
				continue;
			if (!tcp_parse(rxbuf, (unsigned int)len, c->dst_ip,
				       c->dport, c->sport, &s))
				continue;
			if (s.flags & TCP_RST)
				return -2;
			if ((s.flags & (TCP_SYN | TCP_ACK)) ==
			    (TCP_SYN | TCP_ACK) && s.ack == c->snd_nxt + 1) {
				unsigned int mss;

				c->rcv_nxt = s.seq + 1;
				c->snd_nxt++;
				c->snd_una = c->snd_nxt;
				if (tcp_parse_mss(s.tcp, s.doff, &mss) &&
				    mss >= 64 && mss <= TCP_MSS_OURS)
					c->peer_mss = mss;
				if (tcp_send_segment(c, TCP_ACK, c->snd_nxt,
						     c->rcv_nxt, 0, 0, 0) != 0)
					return -3;
				return 0;
			}
		}
	}
	return -1;
}

int tcp_send(struct tcp_conn *c, const void *data, unsigned int len)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rxbuf = netdev_rx_scratch();
	unsigned int tries;

	if (!nd || len > c->peer_mss)
		return -3;

	for (tries = 0; tries < TCP_RETRIES; tries++) {
		unsigned int deadline = get_ticks_ms() + TCP_RETRY_MS;

		if (tcp_send_segment(c, TCP_PSH | TCP_ACK, c->snd_nxt,
				     c->rcv_nxt, (const unsigned char *)data,
				     len, 0) != 0)
			return -3;

		while ((int)(get_ticks_ms() - deadline) < 0) {
			struct tcp_seg s;
			int rlen = nd->rx(rxbuf, NETDEV_FRAME_MAX);

			if (rlen < 0)
				continue;
			if (!tcp_parse(rxbuf, (unsigned int)rlen, c->dst_ip,
				       c->dport, c->sport, &s))
				continue;
			if (s.flags & TCP_RST)
				return -2;
			if (s.ack == c->snd_nxt + len) {
				c->snd_una = s.ack;
				c->snd_nxt = s.ack;
				return 0;
			}
			/* Data arriving while we are sending: buffer it for
			 * tcp_recv (never ACK and drop). */
			if (tcp_rx_data(c, &s) == -2)
				return -2;
		}
	}
	return -1;
}

int tcp_recv(struct tcp_conn *c, unsigned char *out, unsigned int maxlen,
	     unsigned int deadline_ms)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rxbuf = netdev_rx_scratch();

	if (!nd)
		return -2;

	/* Deliver what is already queued (may span several segments). */
	for (;;) {
		struct tcp_seg s;

		if (rxq_len()) {
			unsigned int n = rxq_len();

			if (n > maxlen)
				n = maxlen;
			rxq_get(out, n);
			return (int)n;
		}
		if (c->peer_fin)
			return 0;
		if ((int)(get_ticks_ms() - deadline_ms) >= 0)
			return -1;

		{
			int len = nd->rx(rxbuf, NETDEV_FRAME_MAX);

			if (len < 0)
				continue;
			if (!tcp_parse(rxbuf, (unsigned int)len, c->dst_ip,
				       c->dport, c->sport, &s))
				continue;
		}

		/* Track our send window (nothing pending here, but keeps
		 * snd_una meaningful for the ACKs we emit). */
		if ((unsigned)(s.ack - c->snd_una) <=
		    (unsigned)(c->snd_nxt - c->snd_una))
			c->snd_una = s.ack;

		if (tcp_rx_data(c, &s) == -2)
			return -2;
	}
}

void tcp_close(struct tcp_conn *c)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rxbuf = netdev_rx_scratch();
	unsigned int fin_seq = c->snd_nxt;
	unsigned int tries;

	if (!nd)
		return;

	for (tries = 0; tries < 2; tries++) {
		unsigned int deadline = get_ticks_ms() + 500;
		struct tcp_seg s;

		if (tcp_send_segment(c, TCP_FIN | TCP_ACK, fin_seq,
				     c->rcv_nxt, 0, 0, 0) != 0)
			return;
		c->snd_nxt = fin_seq + 1;

		while ((int)(get_ticks_ms() - deadline) < 0) {
			int len = nd->rx(rxbuf, NETDEV_FRAME_MAX);

			if (len < 0)
				continue;
			if (!tcp_parse(rxbuf, (unsigned int)len, c->dst_ip,
				       c->dport, c->sport, &s))
				continue;
			if (s.flags & TCP_RST)
				return;
			/* Our FIN acked (or their FIN first). */
			if ((s.flags & TCP_FIN) && s.seq == c->rcv_nxt) {
				c->rcv_nxt++;
				c->peer_fin = 1;
				tcp_send_segment(c, TCP_ACK, c->snd_nxt,
						 c->rcv_nxt, 0, 0, 0);
				return;
			}
			if (s.ack == c->snd_nxt)
				return;
		}
	}
}
