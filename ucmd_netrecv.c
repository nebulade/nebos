/* netrecv: ring-3 command.  Listen for 8 s and dump frames. */

#include "ucmd_lib.h"

#define NET_FRAME_MAX 1518

static unsigned char rx_buf[NET_FRAME_MAX];

void _start(const char *arg)
{
	unsigned int deadline, count = 0;

	(void)arg;
	if (u_syscall0(SYS_NET_ACQUIRE) != 0) {
		u_puts("netrecv: no NIC found\n");
		u_exit(1);
	}

	u_puts("netrecv: listening (8s)...\n");
	deadline = u_ticks_ms() + 8000;

	while ((int)(u_ticks_ms() - deadline) < 0) {
		int len = u_syscall2(SYS_NET_RX, (int)rx_buf, NET_FRAME_MAX);
		unsigned int i;

		if (len < 0)
			continue;

		u_syscall2(SYS_NET_DECODE, (int)rx_buf, (unsigned int)len);
		u_putc('\n');

		/* Raw hex dump for closer inspection. */
		for (i = 0; i < (unsigned int)len; i++) {
			u_put_hex2(rx_buf[i]);
			u_putc(' ');
			if ((i & 15) == 15)
				u_putc('\n');
		}
		if (((unsigned int)len & 15) != 0)
			u_putc('\n');

		if (++count >= 32)
			break;
	}

	u_syscall0(SYS_NET_RELEASE);
	u_puts("netrecv: ");
	u_putdec(count);
	u_puts(" frame(s)\n");
	u_exit(0);
}
