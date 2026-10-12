/* Kernel hot-reload over UDP: the host pushes kernel.bin as datagrams, the
 * board writes them into the kexec staging buffer, verifies the checksum and
 * jumps (kexec.c has the serial variant).
 *
 * Protocol (little-endian; UDP to NETKEXEC_PORT; the board replies to the
 * sender's IP/MAC/port):
 *
 *   host -> board  header   "KXNU" entry size csum          (16 bytes)
 *   host -> board  data     "KXND" offset len payload...    (12 + len)
 *   board -> host  ack      "KXNA" next_offset status       (12 bytes)
 *                           status 0 = keep going, 1 = done, 2 = bad checksum
 *
 * The board auto-detects the header from the WM loop (netkexec_pending());
 * program_netkexec_main() then runs stop-and-wait: every datagram is acked
 * with the next expected offset, so a lost or duplicated datagram costs one
 * retransmit.  Datagrams addressed to the broadcast IP are accepted too, so
 * the host can push without knowing whether the board is up yet.
 */

#include "io.h"
#include "kexec.h"
#include "net.h"
#include "netdev.h"
#include "serial.h"
#include "timer.h"
#include "netkexec.h"
#include "mm.h"
#include "ramfs.h"
#include "vfs.h"

#define NETKEXEC_PORT       19288
#define NETKEXEC_DATA_MAX   1400

#define X_MAGIC_HDR   0x554E584Bu   /* "KXNU" */
#define X_MAGIC_PAY   0x504E584Bu   /* "KXNP": a command pack for /ramfs */
#define X_MAGIC_DATA  0x444E584Bu   /* "KXND" */
#define X_MAGIC_ACK   0x414E584Bu   /* "KXNA" */

#define ACK_CONTINUE  0
#define ACK_DONE      1
#define ACK_BADSUM    2

static unsigned char rxbuf[1520];       /* one full Ethernet frame */
static unsigned char replybuf[1520];
static unsigned char ackbuf[12];

/* Page-allocated staging for a payload transfer (freed after the swap). */
static unsigned int pay_buf;
static unsigned int pay_size;

static int net_payload_alloc(unsigned int size)
{
	pay_size = size;
	pay_buf = mm_alloc_pages((size + 0xFFFu) >> 12);
	return pay_buf != 0;
}

static void net_payload_free(void)
{
	unsigned int i;

	for (i = 0; i * 0x1000 < pay_size; i++)
		mm_free_page(pay_buf + i * 0x1000);
	pay_buf = 0;
}

/* Pending transfer (filled by netkexec_pending). */
static int have_header;
static int payload_mode;         /* header was KXNP: replace the ramfs instead */
static unsigned int x_entry, x_size, x_csum;
static unsigned char peer_mac[6], peer_ip[4];
static unsigned int peer_port;

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int ip_eq(const unsigned char *a, const unsigned char *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int ip_is_bcast(const unsigned char *ip)
{
	return ip[0] == 0xFF && ip[1] == 0xFF && ip[2] == 0xFF && ip[3] == 0xFF;
}

/* Parse `f` as a UDP datagram to NETKEXEC_PORT; fill payload + peer address.
 * Returns 1 on match. */
static int parse_udp(const unsigned char *f, unsigned int len,
		     const unsigned char **payload, unsigned int *plen,
		     unsigned char p_mac[6], unsigned char p_ip[4],
		     unsigned int *p_port)
{
	const unsigned char *ip = f + 14;
	const unsigned char *udp;
	unsigned char my_ip[4];
	unsigned int i, ihl, ip_len;

	if (len < 14 + 20 + 8)
		return 0;
	if ((((unsigned int)f[12] << 8) | f[13]) != ET_IPV4)
		return 0;
	if ((ip[0] >> 4) != 4)
		return 0;
	ihl = (unsigned int)(ip[0] & 0x0F) * 4;
	if (ihl < 20 || ip[9] != IPPROTO_UDP)
		return 0;
	ip_len = ((unsigned int)ip[2] << 8) | ip[3];
	if (ip_len < ihl + 8 || 14 + ip_len > len)
		return 0;

	net_get_ip(my_ip);
	if (!ip_eq(ip + 16, my_ip) && !ip_is_bcast(ip + 16))
		return 0;

	udp = ip + ihl;
	if ((((unsigned int)udp[2] << 8) | udp[3]) != NETKEXEC_PORT)
		return 0;

	for (i = 0; i < 6; i++)
		p_mac[i] = f[6 + i];
	for (i = 0; i < 4; i++)
		p_ip[i] = ip[12 + i];
	*p_port = ((unsigned int)udp[0] << 8) | udp[1];
	*payload = udp + 8;
	*plen = ip_len - ihl - 8;
	return *plen > 0;
}

int netkexec_pending(void)
{
	const struct netdev *nd;
	const unsigned char *payload;
	unsigned int plen, port;
	int len;

	if (kexec_busy || netdev_in_use())
		return 0;
	nd = netdev_active();
	if (!nd)
		return 0;

	/* One frame per call; anything else is dropped (this also keeps the RX
	 * ring drained while the system idles at a prompt). */
	len = nd->rx(rxbuf, sizeof(rxbuf));
	if (len < 0)
		return 0;

	if (!parse_udp(rxbuf, (unsigned int)len, &payload, &plen,
		       peer_mac, peer_ip, &port)) {
		/* Not a kexec datagram: stay reachable by answering ARP and ICMP
		 * echo requests for our IP (so the board can be pinged, and a
		 * host can resolve its MAC, without running a command).  Until
		 * an address is configured there is nothing to answer for. */
		int rlen;

		if (!net_is_configured())
			return 0;
		rlen = net_build_reply(rxbuf, (unsigned int)len,
				       replybuf, sizeof(replybuf));
		if (rlen > 0)
			nd->tx(replybuf, (unsigned int)rlen);
		return 0;
	}
	if (plen < 16)
		return 0;
	{
		/* KXNU: kernel image (jump after the checksum); KXNP: a command
		 * pack for /ramfs (swap it in and stay running). */
		unsigned int magic = rd32(payload);

		if (magic != X_MAGIC_HDR && magic != X_MAGIC_PAY)
			return 0;
		payload_mode = (magic == X_MAGIC_PAY);
	}
	x_entry = rd32(payload + 4);
	x_size = rd32(payload + 8);
	x_csum = rd32(payload + 12);
	if (x_size == 0 || x_size > KEXEC_MAX)
		return 0;

	peer_port = port;
	have_header = 1;
	return 1;
}

static void send_ack(unsigned int next_offset, unsigned int status)
{
	wr32(ackbuf, X_MAGIC_ACK);
	wr32(ackbuf + 4, next_offset);
	wr32(ackbuf + 8, status);
	net_udp_send(peer_mac, peer_ip, NETKEXEC_PORT, peer_port,
		     ackbuf, sizeof(ackbuf));
}

static int netkexec_recv(unsigned int stage_base)
{
	volatile unsigned char *stage = (volatile unsigned char *)stage_base;
	unsigned int offset = 0, retries = 0, sum = 0;
	unsigned int deadline = get_ticks_ms() + 3000;

	kexec_total = x_size;
	kexec_progress = 0;
	send_ack(0, ACK_CONTINUE);          /* header ok, send from offset 0 */

	while (offset < x_size) {
		const unsigned char *payload;
		unsigned int plen, port, doff, dlen;
		int len, i;

		if ((int)(get_ticks_ms() - deadline) > 0) {
			if (++retries > 10)
				return -1;      /* host gave up */
			send_ack(offset, ACK_CONTINUE);
			deadline = get_ticks_ms() + 1000;
			continue;
		}

		len = netdev_active()->rx(rxbuf, sizeof(rxbuf));
		if (len < 0)
			continue;
		if (!parse_udp(rxbuf, (unsigned int)len, &payload, &plen,
			       peer_mac, peer_ip, &port))
			continue;
		if (plen < 12 || rd32(payload) != X_MAGIC_DATA)
			continue;

		doff = rd32(payload + 4);
		dlen = rd32(payload + 8);
		if (dlen > NETKEXEC_DATA_MAX || plen < 12 + dlen)
			continue;

		if (doff == offset) {
			for (i = 0; i < (int)dlen; i++) {
				stage[offset + i] = payload[12 + i];
				sum += payload[12 + i];
			}
			offset += dlen;
			kexec_progress = offset;
			retries = 0;
			deadline = get_ticks_ms() + 1000;
			if ((offset & 0x3FFF) == 0)
				io_putchar('.');
		}
		send_ack(offset, ACK_CONTINUE);   /* also acks duplicates */
	}

	if (sum != x_csum) {
		send_ack(offset, ACK_BADSUM);
		return -1;
	}
	send_ack(x_size, ACK_DONE);
	return 0;
}

void program_netkexec_main(void)
{
	if (!have_header) {
		io_putstr("netkexec: no transfer pending\n");
		kexec_busy = 0;
		return;
	}
	have_header = 0;
	netdev_acquire();

	io_putstr("netkexec: receiving ");
	io_puthex(x_size);
	io_putstr(" bytes over UDP...\n");

	if (payload_mode) {
		unsigned int buf = 0;

		if (!net_payload_alloc(x_size)) {
			io_putstr("netkexec: out of memory (staging)\n");
			netdev_release();
			kexec_busy = 0;
			return;
		}
		buf = pay_buf;

		if (netkexec_recv(buf) != 0) {
			io_putstr("\nnetkexec: aborted (timeout or checksum)\n");
			net_payload_free();
			netdev_release();
			kexec_busy = 0;
			return;
		}

		io_putstr("\nnetkexec: received ");
		io_puthex(x_size);
		io_putstr(" bytes, ");
		if (ramfs_replace((const void *)buf, x_size) != 0) {
			io_putstr("not a valid pack (unchanged)\n");
			net_payload_free();     /* the ramfs kept the old pack */
		} else {
			vfs_refresh_ramfs();
			io_putstr("pack installed (");
			io_puthex(ramfs_count());
			io_putstr(" files)\n");
			pay_buf = 0;            /* ownership moved to the ramfs */
			pay_size = 0;
		}
		netdev_release();
		kexec_busy = 0;
		return;
	}

	if (!kexec_stage_alloc(x_size)) {
		io_putstr("netkexec: out of memory (staging)\n");
		netdev_release();
		kexec_busy = 0;
		return;
	}

	if (netkexec_recv(kexec_stage) != 0) {
		io_putstr("\nnetkexec: aborted (timeout or checksum)\n");
		kexec_stage_free();
		netdev_release();
		kexec_busy = 0;
		return;
	}

	io_putstr("\nnetkexec: received ");
	io_puthex(x_size);
	io_putstr(" bytes, entry ");
	io_puthex(x_entry);
	io_putstr(" -> jumping\n");

	kexec_jump(x_entry, x_size);     /* never returns */
}
