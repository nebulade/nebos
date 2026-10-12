/* pong: ring-3 command.  Answer ARP/ICMP for our IP for 30 s. */

#include "ucmd_lib.h"

#define NET_FRAME_MAX 1518
#define NETLOG_PORT 19290

static unsigned char rx_buf[NET_FRAME_MAX];
static unsigned char reply_buf[NET_FRAME_MAX];

void _start(const char *arg)
{
	unsigned char ip[4];
	unsigned int deadline, count = 0;

	(void)arg;
	if (u_syscall0(SYS_NET_ACQUIRE) != 0) {
		u_puts("pong: no NIC found\n");
		u_exit(1);
	}

	u_syscall1(SYS_NET_GET_IP, (int)ip);
	u_puts("pong: answering ARP/ICMP for 30s (ip=");
	u_put_ip(ip);
	u_puts(")...\n");

	deadline = u_ticks_ms() + 30000;
	while ((int)(u_ticks_ms() - deadline) < 0) {
		int len = u_syscall2(SYS_NET_RX, (int)rx_buf, NET_FRAME_MAX);
		int rlen;

		if (len < 0)
			continue;

		rlen = u_syscall3(SYS_NET_REPLY, (int)rx_buf, (unsigned int)len,
				  (int)reply_buf);
		if (rlen > 0) {
			if (u_syscall2(SYS_NET_TX, (int)reply_buf,
				       (unsigned int)rlen) == 0) {
				u_puts("reply: ");
				u_syscall2(SYS_NET_DECODE, (int)reply_buf,
					   (unsigned int)rlen);
				u_putc('\n');
				count++;
			}
		}
	}

	u_syscall0(SYS_NET_RELEASE);
	u_puts("pong: done (");
	u_putdec(count);
	u_puts(" replies)\n");
	u_exit(0);
}
