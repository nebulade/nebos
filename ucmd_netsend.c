/* netsend: ring-3 command.  Broadcast a 64-byte test frame. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	unsigned char frame[64], mac[6];
	unsigned int i;

	(void)arg;
	if (u_syscall1(SYS_NET_MAC, (int)mac) != 0) {
		u_puts("netsend: no NIC found\n");
		u_exit(1);
	}

	/* dst=ff:ff:ff:ff:ff:ff, src=our MAC, ethertype 0x88B5 (local
	 * experimental), payload "NEBS". */
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

	u_syscall0(SYS_NET_ACQUIRE);
	if (u_syscall2(SYS_NET_TX, (int)frame, sizeof(frame)) != 0) {
		u_puts("netsend: transmit failed\n");
		u_syscall0(SYS_NET_RELEASE);
		u_exit(1);
	}
	u_syscall0(SYS_NET_RELEASE);
	u_puts("netsend: sent 64-byte broadcast frame\n");
	u_exit(0);
}
