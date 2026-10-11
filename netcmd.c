/* Network test commands: netinfo, netrecv, netsend, netip, ping. */

#include "io.h"
#include "timer.h"
#include "ne2000.h"
#include "net.h"

/* Shared frame buffers (file scope, not stack: the 4 KB process stack can't
   hold a full-size frame, and keeping them out of BSS-as-much-as-possible
   avoids bloating the kernel past its 0x40000 limit). Max Ethernet frame is
   1518 bytes (14 header + 1500 payload + 4 VLAN). */
#define NET_FRAME_MAX 1518
static unsigned char net_rx_buf[NET_FRAME_MAX];
static unsigned char net_reply_buf[NET_FRAME_MAX];

static void put_hex2(unsigned char v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
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

void program_netinfo_main(void)
{
	ne2000_init();
	ne2000_print_info();
	if (ne2000_present()) {
		io_putstr("ip=");
		net_print_ip();
		io_putchar('\n');
	}
}

void program_netrecv_main(void)
{
	unsigned int deadline, count = 0;

	ne2000_init();
	if (!ne2000_present()) {
		io_putstr("netrecv: no NIC found\n");
		return;
	}

	io_putstr("netrecv: listening (8s)...\n");
	deadline = get_ticks_ms() + 8000;

	while (get_ticks_ms() < deadline) {
		int len = ne2000_rx(net_rx_buf, NET_FRAME_MAX);
		unsigned int i;

		if (len < 0)
			continue;

		net_decode_frame(net_rx_buf, (unsigned int)len);
		io_putchar('\n');

		/* Raw hex dump for closer inspection. */
		for (i = 0; i < (unsigned int)len; i++) {
			put_hex2(net_rx_buf[i]);
			io_putchar(' ');
			if ((i & 15) == 15)
				io_putchar('\n');
		}
		if (((unsigned int)len & 15) != 0)
			io_putchar('\n');

		if (++count >= 32)
			break;
	}

	io_putstr("netrecv: ");
	put_dec(count);
	io_putstr(" frame(s)\n");
}

void program_netsend_main(void)
{
	unsigned char frame[64];
	unsigned char mac[6];
	unsigned int i;

	ne2000_init();
	if (!ne2000_present()) {
		io_putstr("netsend: no NIC found\n");
		return;
	}

	ne2000_get_mac(mac);

	/* Broadcast test frame: dst=ff:ff:ff:ff:ff:ff, src=our MAC,
	   ethertype 0x88B5 (local experimental), payload "NEBS". */
	for (i = 0; i < 6; i++)
		frame[i] = 0xFF;
	for (i = 0; i < 6; i++)
		frame[6 + i] = mac[i];
	frame[12] = 0x88;
	frame[13] = 0xB5;
	frame[14] = 'N';
	frame[15] = 'E';
	frame[16] = 'B';
	frame[17] = 'S';
	for (i = 18; i < 64; i++)
		frame[i] = 0x00;

	if (ne2000_tx(frame, sizeof(frame)) != 0) {
		io_putstr("netsend: transmit failed\n");
		return;
	}
	io_putstr("netsend: sent 64-byte broadcast frame\n");
}

void program_netip_main(const char *arg)
{
	unsigned char ip[4];

	if (arg[0] == '\0') {
		net_print_ip();
		io_putchar('\n');
		return;
	}

	if (net_parse_ip(arg, ip) != 0) {
		io_putstr("netip: usage netip <a.b.c.d>\n");
		return;
	}

	net_set_ip(ip);
	io_putstr("ip set to ");
	net_print_ip();
	io_putchar('\n');
}

void program_ping_main(void)
{
	unsigned int deadline, count = 0;

	ne2000_init();
	if (!ne2000_present()) {
		io_putstr("ping: no NIC found\n");
		return;
	}

	io_putstr("ping: answering ARP/ICMP for 30s (ip=");
	net_print_ip();
	io_putstr(")...\n");

	deadline = get_ticks_ms() + 30000;
	while (get_ticks_ms() < deadline) {
		int len = ne2000_rx(net_rx_buf, NET_FRAME_MAX);
		int rlen;

		if (len < 0)
			continue;

		rlen = net_build_reply(net_rx_buf, (unsigned int)len,
				       net_reply_buf, NET_FRAME_MAX);
		if (rlen > 0) {
			if (ne2000_tx(net_reply_buf, (unsigned int)rlen) == 0) {
				io_putstr("reply: ");
				net_decode_frame(net_reply_buf, (unsigned int)rlen);
				io_putchar('\n');
				count++;
			}
		}
	}

	io_putstr("ping: done (");
	put_dec(count);
	io_putstr(" replies)\n");
}
