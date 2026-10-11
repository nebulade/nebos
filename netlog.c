/* netlog: see netlog.h. */

#include "net.h"
#include "netdev.h"
#include "timer.h"
#include "netlog.h"

#define NETLOG_RING      2048   /* producer/consumer byte ring */
#define NETLOG_FLUSH_MIN 256    /* flush even without a newline at this size */
#define NETLOG_IDLE_MS   100    /* flush a partial line after this idle time */

static volatile unsigned char ring[NETLOG_RING];
static volatile unsigned int head;
static volatile unsigned int tail;
static volatile unsigned int dropped;
static volatile unsigned int last_append_ms;

static int enabled;
static unsigned char host_ip[4] = { 255, 255, 255, 255 };

void netlog_init(void)
{
	head = tail = 0;
	dropped = 0;
	last_append_ms = get_ticks_ms();
	enabled = 1;
}

void netlog_set_enabled(int on)
{
	enabled = on ? 1 : 0;
}

int netlog_get_enabled(void)
{
	return enabled;
}

void netlog_set_host(const unsigned char ip[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		host_ip[i] = ip[i];
}

void netlog_get_host(unsigned char ip[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		ip[i] = host_ip[i];
}

void netlog_char(char c)
{
	unsigned int next;

	if (!enabled)
		return;
	next = (head + 1) % NETLOG_RING;
	if (next == tail) {
		dropped++;              /* full: drop, never block */
		return;
	}
	ring[head] = (unsigned char)c;
	head = next;
	last_append_ms = get_ticks_ms();
}

static unsigned int pending(void)
{
	if (head >= tail)
		return head - tail;
	return NETLOG_RING - tail + head;
}

static void send_line(const unsigned char *buf, unsigned int len)
{
	static const unsigned char bcast_mac[6] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	unsigned char dst[4];

	netlog_get_host(dst);
	net_udp_send(bcast_mac, dst, NETLOG_PORT, NETLOG_PORT, buf, len);
}

void netlog_flush(void)
{
	unsigned char buf[NET_UDP_MAX];
	unsigned int n = 0, avail, idle, now;
	static const char dropmsg[] = "[netlog: output dropped, buffer full]\n";
	unsigned int i;

	if (!enabled || netdev_in_use() || !netdev_active())
		return;

	avail = pending();
	if (avail == 0)
		return;

	now = get_ticks_ms();
	idle = now - last_append_ms;
	if (avail < NETLOG_FLUSH_MIN && idle < NETLOG_IDLE_MS)
		return;                 /* wait for a complete line */

	if (dropped) {
		send_line((const unsigned char *)dropmsg, sizeof(dropmsg) - 1);
		dropped = 0;
	}

	/* One datagram, ending at a newline when possible. */
	while (n < NET_UDP_MAX && tail != head) {
		unsigned char c = ring[tail];

		tail = (tail + 1) % NETLOG_RING;
		buf[n++] = c;
		if (c == '\n')
			break;
	}
	if (n == 0)
		return;
	send_line(buf, n);
	(void)i;
}
