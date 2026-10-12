/* netloop: ring-3 command.  PHY loopback self-test (no cable/peer needed). */

#include "ucmd_lib.h"

#define NET_FRAME_MAX 1518

static unsigned char rx_buf[NET_FRAME_MAX];

void _start(const char *arg)
{
	unsigned char frame[64], mac[6];
	unsigned int i, deadline;
	int got = 0;

	(void)arg;
	if (u_syscall1(SYS_NET_MAC, (int)mac) != 0) {
		u_puts("netloop: no NIC found\n");
		u_exit(1);
	}
	if (u_syscall1(SYS_NET_LOOPBACK, 1) != 0) {
		u_puts("netloop: not supported by this NIC\n");
		u_exit(1);
	}

	for (i = 0; i < 6; i++)
		frame[i] = 0xFF;                      /* broadcast */
	for (i = 0; i < 6; i++)
		frame[6 + i] = mac[i];
	frame[12] = 0x88;
	frame[13] = 0xB6;                             /* local experimental */
	frame[14] = 'N'; frame[15] = 'E'; frame[16] = 'B';
	frame[17] = 'O'; frame[18] = 'S'; frame[19] = 'L';
	frame[20] = 'O'; frame[21] = 'O';
	for (i = 22; i < 64; i++)
		frame[i] = 0x00;

	u_puts("netloop: PHY loopback on, sending...\n");
	u_syscall0(SYS_NET_ACQUIRE);
	if (u_syscall2(SYS_NET_TX, (int)frame, sizeof(frame)) != 0) {
		u_puts("netloop: transmit failed\n");
		u_syscall0(SYS_NET_RELEASE);
		u_syscall1(SYS_NET_LOOPBACK, 0);
		u_exit(1);
	}

	deadline = u_ticks_ms() + 1000;
	while ((int)(u_ticks_ms() - deadline) < 0) {
		int len = u_syscall2(SYS_NET_RX, (int)rx_buf, NET_FRAME_MAX);

		if (len < 0)
			continue;
		if (len >= 22 &&
		    rx_buf[12] == 0x88 && rx_buf[13] == 0xB6 &&
		    rx_buf[14] == 'N' && rx_buf[15] == 'E' &&
		    rx_buf[16] == 'B' && rx_buf[17] == 'O' &&
		    rx_buf[18] == 'S') {
			got = 1;
			break;
		}
	}

	u_syscall0(SYS_NET_RELEASE);
	u_syscall1(SYS_NET_LOOPBACK, 0);
	u_puts(got ? "netloop: PASS (frame came back)\n"
		   : "netloop: FAIL (no frame came back)\n");
	u_exit(got ? 0 : 1);
}
