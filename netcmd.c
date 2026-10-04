/* Network test commands: netinfo, netrecv, netsend. */

#include "io.h"
#include "timer.h"
#include "ne2000.h"

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
}

void program_netrecv_main(void)
{
	unsigned char buf[2048];
	unsigned int deadline, count = 0;

	ne2000_init();
	if (!ne2000_present()) {
		io_putstr("netrecv: no NIC found\n");
		return;
	}

	io_putstr("netrecv: listening (8s)...\n");
	deadline = get_ticks_ms() + 8000;

	while (get_ticks_ms() < deadline) {
		int len = ne2000_rx(buf, sizeof(buf));
		unsigned int i;

		if (len < 0)
			continue;

		io_putstr("rx ");
		put_dec((unsigned int)len);
		io_putstr(" bytes:\n");
		for (i = 0; i < (unsigned int)len; i++) {
			put_hex2(buf[i]);
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
