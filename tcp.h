/* Minimal TCP client (RFC 793 subset) for outgoing connections.
 *
 * Single connection, in-order receive only, no congestion control, no
 * window scaling, no out-of-order reassembly, no fragmentation.  Everything
 * is polled from the netdev rx() call, matching the rest of the kernel.
 * The caller owns the NIC (netdev_acquire) from connect until close. */
#ifndef TCP_H
#define TCP_H

struct tcp_conn {
	unsigned char dst_mac[6];
	unsigned char dst_ip[4];
	unsigned int  sport, dport;
	unsigned int  snd_una;    /* oldest unacknowledged sequence */
	unsigned int  snd_nxt;    /* next sequence number to send */
	unsigned int  rcv_nxt;    /* next sequence number expected from peer */
	unsigned int  peer_mss;   /* from the SYN-ACK option; 536 when absent */
	unsigned int  peer_wnd;   /* peer's advertised receive window (raw) */
	int           peer_fin;   /* peer FIN consumed (EOF) */
	int           need_wnd;   /* re-send the window update after the final ACK */
};

/* Open a connection (SYN -> SYN-ACK -> ACK).  Returns:
 *   0 = established,
 *  -1 = no reply (timeout),
 *  -2 = refused (RST),
 *  -3 = transmit failed / no NIC / no IP. */
int tcp_connect(const unsigned char *dst_mac, const unsigned char *dst_ip,
		unsigned int dport, struct tcp_conn *c);

/* Send one segment (len must be <= peer_mss), retransmitting until acked.
 * Returns 0, or -1 timeout / -2 reset / -3 transmit failed. */
int tcp_send(struct tcp_conn *c, const void *data, unsigned int len);

/* Read the next in-order payload bytes into out.  Returns >0 = byte count,
 * 0 = peer closed (EOF), -1 = deadline passed, -2 = reset. */
int tcp_recv(struct tcp_conn *c, unsigned char *out, unsigned int maxlen,
	     unsigned int deadline_ms);

/* Send FIN and drain the close handshake briefly (best effort). */
void tcp_close(struct tcp_conn *c);

/* ---- Server side (WebDAV listener) ----
 *
 * One listening port, one active connection and a small SYN backlog.  The
 * caller owns the NIC.  Incoming frames are fed to tcp_srv_input() (which
 * also answers SYNs and RSTs unknown ports); tcp_srv_accept() promotes a
 * backlog connection for send/recv with the normal tcp_send()/tcp_recv(). */
void tcp_srv_init(unsigned int port);
void tcp_srv_reset(void);
/* Handle one frame.  Returns 1 when it was a TCP frame for us (consumed),
 * 0 when it is not TCP / not addressed to us (the caller may answer ARP). */
int  tcp_srv_input(const unsigned char *frame, unsigned int len);
/* Promote the oldest pending connection (NULL when none is ready). */
struct tcp_conn *tcp_srv_accept(void);
/* Send len bytes, split to the peer's MSS.  0 / -1 / -2 / -3. */
int  tcp_srv_send(struct tcp_conn *c, const void *data, unsigned int len);
/* FIN once, then remember the tuple briefly so a late FIN/ACK is absorbed
 * instead of answered with RST. */
void tcp_srv_close(struct tcp_conn *c);

#endif
