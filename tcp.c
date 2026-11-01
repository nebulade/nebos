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
#define TCP_RETRY_MS  500
#define TCP_RETRIES   3

/* Listening port for the server side (0 = not listening).  Declared here
 * because the shared receive loop feeds non-matching frames to the listener. */
static unsigned int srv_port;

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
static int tcp_send_segment_wnd(struct tcp_conn *c, unsigned char flags,
				unsigned int seq, unsigned int ack,
				const unsigned char *payload, unsigned int len,
				int with_mss, unsigned int wnd)
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
	wr16(tcp + 14, wnd > 0xFFFFu ? 0xFFFFu : wnd);
	wr16(tcp + 16, 0);                /* checksum */
	wr16(tcp + 18, 0);                /* urgent pointer */
	for (i = 0; i < len; i++)
		tcp[20 + optlen + i] = payload[i];

	cs = net_checksum_tcp(my_ip, c->dst_ip, tcp, 20 + optlen + len);
	wr16(tcp + 16, cs);

	return nd->tx(tx_frame, 14 + total);
}

static int tcp_send_segment(struct tcp_conn *c, unsigned char flags,
			    unsigned int seq, unsigned int ack,
			    const unsigned char *payload, unsigned int len,
			    int with_mss)
{
	unsigned int wnd = with_mss ? TCP_WINDOW : rxq_space();

	return tcp_send_segment_wnd(c, flags, seq, ack, payload, len,
				    with_mss, wnd);
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
	if (s->tcp)
		c->peer_wnd = rd16(s->tcp + 14);

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
	c->peer_wnd = 4096;
	c->peer_fin = 0;
	c->need_wnd = 0;
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
				c->peer_wnd = rd16(s.tcp + 14);
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
			/* Window update: ACKs sent while the queue was full may
			 * have advertised zero, and without this the peer would
			 * only recover on its next zero-window probe. */
			tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
					 0, 0, 0);
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
				       c->dport, c->sport, &s)) {
				/* A new connection (or its first data) while
				 * this one is being served. */
				if (srv_port)
					tcp_srv_input(rxbuf, (unsigned int)len);
				continue;
			}
		}

		/* Track our send window (nothing pending here, but keeps
		 * snd_una meaningful for the ACKs we emit). */
		if ((unsigned)(s.ack - c->snd_una) <=
		    (unsigned)(c->snd_nxt - c->snd_una))
			c->snd_una = s.ack;
		if (c->need_wnd && (s.flags & TCP_ACK)) {
			tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
					 0, 0, 0);
			c->need_wnd = 0;
		}

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

/* ---- Server side ----
 *
 * One listening port, one active connection, two slots that hold a
 * handshake-complete connection until the command gets around to accepting
 * it.  A closed connection lingers for a few seconds so the peer's FIN/ACK
 * is absorbed instead of answered with RST (libsoup treats a reset after a
 * complete response as an error). */

#define TCP_SRV_SLOTS      4          /* 1 active + 3 backlog/closing */
#define TCP_SRV_CLOSE_MS   2000
#define TCP_SRV_BACKLOG_MS 30000

enum {
	SRV_FREE = 0,
	SRV_BACKLOG,
	SRV_ACTIVE,
	SRV_CLOSING
};

#define SRV_PRE_MAX 1024

struct tcp_srv {
	struct tcp_conn c;
	int used;
	int state;
	unsigned int last_ms;
	unsigned char pre[SRV_PRE_MAX];   /* first request, held until accept */
	unsigned int pre_len;
};

static struct tcp_srv srv[TCP_SRV_SLOTS];
static int srv_active = -1;

struct tcp_rx {
	unsigned int seq, ack;
	unsigned char flags;
	const unsigned char *data;
	unsigned int len;
	const unsigned char *tcp;
	unsigned int doff;
	unsigned char src_mac[6];
	unsigned char src_ip[4];
	unsigned int sport, dport;
};

/* Parse any IPv4/TCP frame addressed to us (no connection match). */
static int tcp_parse_any(const unsigned char *f, unsigned int flen,
			 struct tcp_rx *s)
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
	if ((ip[6] & 0x3F) != 0 || ip[7] != 0)
		return 0;
	ip_len = rd16(ip + 2);
	if (ip_len < ihl + 20 || ip_len > flen - 14)
		return 0;

	net_get_ip(my_ip);
	for (i = 0; i < 4; i++)
		if (ip[16 + i] != my_ip[i])
			return 0;

	tcp = ip + ihl;
	doff = (unsigned int)(tcp[12] >> 4) * 4;
	if (doff < 20 || ihl + doff > ip_len)
		return 0;

	for (i = 0; i < 6; i++)
		s->src_mac[i] = f[6 + i];
	for (i = 0; i < 4; i++)
		s->src_ip[i] = ip[12 + i];
	s->sport = rd16(tcp);
	s->dport = rd16(tcp + 2);
	s->seq = rd32(tcp + 4);
	s->ack = rd32(tcp + 8);
	s->flags = tcp[13];
	s->data = tcp + doff;
	s->len = ip_len - ihl - doff;
	s->tcp = tcp;
	s->doff = doff;
	return 1;
}

static int srv_match(const struct tcp_conn *c, const struct tcp_rx *s)
{
	unsigned int i;

	if (s->sport != c->dport || s->dport != c->sport)
		return 0;
	for (i = 0; i < 4; i++)
		if (s->src_ip[i] != c->dst_ip[i])
			return 0;
	return 1;
}

static void srv_clear(int i)
{
	srv[i].used = 0;
	srv[i].state = SRV_FREE;
	srv[i].pre_len = 0;
}

static void srv_expire(void)
{
	unsigned int now = get_ticks_ms();
	int i;

	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		if (!srv[i].used)
			continue;
		if (srv[i].state == SRV_CLOSING &&
		    now - srv[i].last_ms >= TCP_SRV_CLOSE_MS) {
			srv_clear(i);
			if (srv_active == i)
				srv_active = -1;
		} else if (srv[i].state == SRV_BACKLOG &&
			   now - srv[i].last_ms >= TCP_SRV_BACKLOG_MS) {
			srv_clear(i);
		}
	}
}

static unsigned int srv_isn(void)
{
	return (get_ticks_ms() * 1103515245u) ^ 0x5EED0000u;
}

static void srv_resend_synack(struct tcp_srv *e)
{
	tcp_send_segment_wnd(&e->c, TCP_SYN | TCP_ACK, e->c.snd_una - 1,
			     e->c.rcv_nxt, 0, 0, 1, 0);
}

/* Build a connection from a SYN and answer SYN-ACK. */
static int srv_new(const struct tcp_rx *s)
{
	struct tcp_srv *e;
	unsigned int mss = 536, i;
	int slot = -1;

	/* Retransmitted SYN: answer with the same ISN. */
	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		if (!srv[i].used || srv[i].state == SRV_CLOSING)
			continue;
		if (srv_match(&srv[i].c, s)) {
			srv_resend_synack(&srv[i]);
			return 1;
		}
	}

	srv_expire();
	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		/* A connection that already sent its response is finished
		 * as far as new clients are concerned: reuse the slot rather
		 * than make the next SYN wait for the linger to expire. */
		if (!srv[i].used || srv[i].state == SRV_CLOSING) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0)
		return 1;                 /* no room: drop, the client retries */

	e = &srv[slot];
	for (i = 0; i < 6; i++)
		e->c.dst_mac[i] = s->src_mac[i];
	for (i = 0; i < 4; i++)
		e->c.dst_ip[i] = s->src_ip[i];
	e->c.sport = srv_port;
	e->c.dport = s->sport;
	e->c.snd_una = e->c.snd_nxt = srv_isn();
	e->c.rcv_nxt = s->seq + 1;
	e->c.peer_fin = 0;
	e->c.need_wnd = 0;
	e->c.peer_mss = 536;
	e->c.peer_wnd = rd16(s->tcp + 14);
	if (!e->c.peer_wnd)
		e->c.peer_wnd = 4096;
	if (tcp_parse_mss(s->tcp, s->doff, &mss) &&
	    mss >= 64 && mss <= TCP_MSS_OURS)
		e->c.peer_mss = mss;
	e->used = 1;
	e->state = SRV_BACKLOG;
	e->last_ms = get_ticks_ms();
	e->pre_len = 0;

	tcp_send_segment_wnd(&e->c, TCP_SYN | TCP_ACK, e->c.snd_nxt,
			     e->c.rcv_nxt, 0, 0, 1, 0);
	/* The SYN consumed one sequence number. */
	e->c.snd_nxt++;
	e->c.snd_una = e->c.snd_nxt;
	return 1;
}

static void srv_send_rst(const struct tcp_rx *s)
{
	struct tcp_conn c;
	unsigned int ack = s->seq + s->len;
	unsigned int i;

	if (s->flags & TCP_SYN)
		ack++;
	if (s->flags & TCP_FIN)
		ack++;

	for (i = 0; i < 6; i++)
		c.dst_mac[i] = s->src_mac[i];
	for (i = 0; i < 4; i++)
		c.dst_ip[i] = s->src_ip[i];
	c.sport = s->dport;
	c.dport = s->sport;
	c.snd_una = c.snd_nxt = s->ack;
	c.rcv_nxt = ack;
	c.peer_mss = 536;
	c.peer_fin = 0;
	tcp_send_segment(&c, TCP_RST | TCP_ACK, s->ack, ack, 0, 0, 0);
}

static void srv_rx_data(struct tcp_conn *c, const struct tcp_rx *r)
{
	struct tcp_seg s;

	s.seq = r->seq;
	s.ack = r->ack;
	s.flags = r->flags;
	s.data = r->data;
	s.len = r->len;
	s.tcp = r->tcp;
	s.doff = r->doff;
	tcp_rx_data(c, &s);
}

void tcp_srv_init(unsigned int port)
{
	int i;

	for (i = 0; i < TCP_SRV_SLOTS; i++)
		srv[i].used = 0, srv[i].state = SRV_FREE;
	srv_active = -1;
	srv_port = port;
}

void tcp_srv_reset(void)
{
	tcp_srv_init(0);
	rxq_head = rxq_tail = 0;
}

int tcp_srv_input(const unsigned char *f, unsigned int len)
{
	struct tcp_rx s;
	int i;

	if (!srv_port || !tcp_parse_any(f, len, &s))
		return 0;

	/* The accepted connection. */
	if (srv_active >= 0 && srv_match(&srv[srv_active].c, &s)) {
		struct tcp_conn *c = &srv[srv_active].c;

		if (c->need_wnd && (s.flags & TCP_ACK)) {
			tcp_send_segment(c, TCP_ACK, c->snd_nxt, c->rcv_nxt,
					 0, 0, 0);
			c->need_wnd = 0;
		}
		if (s.flags & TCP_RST) {
			srv_clear(srv_active);
			srv_active = -1;
			return 1;
		}
		if ((unsigned)(s.ack - c->snd_una) <=
		    (unsigned)(c->snd_nxt - c->snd_una))
			c->snd_una = s.ack;
		srv_rx_data(c, &s);
		srv[srv_active].last_ms = get_ticks_ms();
		return 1;
	}

	/* A pending, closing or duplicate connection. */
	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		if (!srv[i].used || !srv_match(&srv[i].c, &s))
			continue;
		if (srv[i].state == SRV_CLOSING) {
			if (s.flags & TCP_FIN) {
				srv[i].c.rcv_nxt++;
				tcp_send_segment(&srv[i].c, TCP_ACK,
						 srv[i].c.snd_nxt,
						 srv[i].c.rcv_nxt, 0, 0, 0);
			}
			if (s.flags & TCP_RST)
				srv_clear(i);
			return 1;
		}
		if (s.flags & TCP_RST) {
			srv_clear(i);
			return 1;
		}
		if ((s.flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
			srv_resend_synack(&srv[i]);
			return 1;
		}
		if ((unsigned)(s.ack - srv[i].c.snd_una) <=
		    (unsigned)(srv[i].c.snd_nxt - srv[i].c.snd_una))
			srv[i].c.snd_una = s.ack;

		/* Hold the first request (small HTTP headers) until the
		 * command accepts this connection.  ACK it so the client does
		 * not have to wait for an RTO; if it does not fit, leave it
		 * unacknowledged and the client retransmits after accept. */
		if (s.len && s.seq == srv[i].c.rcv_nxt &&
		    s.len <= SRV_PRE_MAX - srv[i].pre_len) {
			unsigned int k;

			for (k = 0; k < s.len; k++)
				srv[i].pre[srv[i].pre_len + k] = s.data[k];
			srv[i].pre_len += s.len;
			srv[i].c.rcv_nxt += s.len;
			if ((s.flags & TCP_FIN) &&
			    s.seq + s.len == srv[i].c.rcv_nxt) {
				srv[i].c.peer_fin = 1;
				srv[i].c.rcv_nxt++;
			}
			/* Keep the queued connection's window closed so it
			 * does not send a body we have nowhere to put. */
			tcp_send_segment_wnd(&srv[i].c, TCP_ACK,
					     srv[i].c.snd_nxt, srv[i].c.rcv_nxt,
					     0, 0, 0, 0);
		}
		srv[i].last_ms = get_ticks_ms();
		return 1;
	}

	/* A new connection. */
	if ((s.flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
	    s.dport == srv_port)
		return srv_new(&s);

	/* TCP for us on a port we do not serve: refuse it. */
	srv_send_rst(&s);
	return 1;
}

struct tcp_conn *tcp_srv_accept(void)
{
	int i, best = -1;

	if (srv_active >= 0)
		return &srv[srv_active].c;

	srv_expire();
	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		if (!srv[i].used || srv[i].state != SRV_BACKLOG)
			continue;
		if (best < 0 ||
		    (int)(srv[i].last_ms - srv[best].last_ms) < 0)
			best = i;
	}
	if (best < 0)
		return 0;

	srv[best].state = SRV_ACTIVE;
	srv_active = best;
	rxq_head = rxq_tail = 0;
	if (srv[best].pre_len) {
		rxq_put(srv[best].pre, srv[best].pre_len);
		srv[best].pre_len = 0;
	}
	/* The SYN-ACK advertised a zero window while this connection waited
	 * for the command; open it now with the real queue space. */
	tcp_send_segment(&srv[best].c, TCP_ACK, srv[best].c.snd_nxt,
			 srv[best].c.rcv_nxt, 0, 0, 0);
	/* If this update raced ahead of the client's final ACK it may have
	 * been discarded; repeat it when that ACK reaches us. */
	srv[best].c.need_wnd = 1;
	return &srv[best].c;
}

/* Send up to four segments before waiting for the cumulative ACK.  Linux
 * delays the ACK of a lone small segment by up to 40 ms; a burst makes the
 * peer acknowledge every second segment immediately, which is the difference
 * between ~35 KB/s and a few MB/s on a LAN.  The peer's advertised window
 * caps the burst. */
int tcp_srv_send(struct tcp_conn *c, const void *data, unsigned int len)
{
	const struct netdev *nd = netdev_active();
	unsigned char *rxbuf = netdev_rx_scratch();
	const unsigned char *p = data;
	unsigned int mss;

	if (!c || !nd)
		return -3;
	mss = c->peer_mss;
	if (mss < 64)
		mss = 64;

	while (len) {
		struct {
			unsigned int seq;
			unsigned int len;
			const unsigned char *data;
		} seg[4];
		unsigned int nseg = 0, off = 0, total = 0, cap, wnd;
		unsigned int tries, i;

		wnd = c->peer_wnd ? c->peer_wnd : 4096;
		cap = mss * 4;
		if (cap > wnd)
			cap = wnd;
		if (cap < mss)
			cap = mss;

		while (off < len && nseg < 4 && total < cap) {
			unsigned int n = len - off;

			if (n > mss)
				n = mss;
			if (total + n > cap)
				n = cap - total;
			if (!n)
				break;
			seg[nseg].seq = c->snd_nxt;
			seg[nseg].len = n;
			seg[nseg].data = p + off;
			if (tcp_send_segment(c, TCP_PSH | TCP_ACK, c->snd_nxt,
					     c->rcv_nxt, p + off, n, 0) != 0)
				return -3;
			c->snd_nxt += n;
			off += n;
			total += n;
			nseg++;
		}

		for (tries = 0; ; tries++) {
			unsigned int deadline = get_ticks_ms() + TCP_RETRY_MS;

			while ((int)(get_ticks_ms() - deadline) < 0) {
				struct tcp_seg s;
				int rlen = nd->rx(rxbuf, NETDEV_FRAME_MAX);

				if (rlen < 0)
					continue;
				if (!tcp_parse(rxbuf, (unsigned int)rlen,
					       c->dst_ip, c->dport, c->sport, &s)) {
					if (srv_port)
						tcp_srv_input(rxbuf,
							      (unsigned int)rlen);
					continue;
				}
				if (s.flags & TCP_RST)
					return -2;
				if ((unsigned)(s.ack - c->snd_una) <=
				    (unsigned)(c->snd_nxt - c->snd_una))
					c->snd_una = s.ack;
				c->peer_wnd = rd16(s.tcp + 14);
				if ((int)(s.ack - c->snd_nxt) >= 0)
					goto burst_done;
				tcp_rx_data(c, &s);
			}
			if (tries >= TCP_RETRIES)
				return -1;
			/* Retransmit the still-unacknowledged part of this
			 * burst (duplicates are harmless). */
			for (i = 0; i < nseg; i++) {
				if ((int)(seg[i].seq + seg[i].len -
					  c->snd_una) <= 0)
					continue;
				tcp_send_segment(c, TCP_PSH | TCP_ACK,
						 seg[i].seq, c->rcv_nxt,
						 seg[i].data, seg[i].len, 0);
			}
		}
burst_done:
		p += off;
		len -= off;
	}
	return 0;
}

void tcp_srv_close(struct tcp_conn *c)
{
	int i;

	if (!c)
		return;
	for (i = 0; i < TCP_SRV_SLOTS; i++) {
		if (!srv[i].used || &srv[i].c != c)
			continue;
		tcp_send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, c->rcv_nxt,
				 0, 0, 0);
		c->snd_nxt++;
		srv[i].state = SRV_CLOSING;
		srv[i].last_ms = get_ticks_ms();
		if (srv_active == i)
			srv_active = -1;
		return;
	}
}
