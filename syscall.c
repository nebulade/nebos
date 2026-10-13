/* Syscall dispatcher: services int 0x80 requests from ring-3. */

#include "syscall.h"
#include "io.h"
#include "process.h"
#include "rtc.h"
#include "serial.h"
#include "sys_fs.h"
#include "sys_gfx.h"
#include "sys_info.h"
#include "sys_net.h"
#include "timer.h"

int syscall_dispatch(int num, int a1, int a2, int a3)
{
	switch (num) {
	case SYS_PUTSTR:
		io_putstr((const char *)a1);
		return 0;
	case SYS_GET_TICKS_MS:
		return (int)get_ticks_ms();
	case SYS_SLEEP_MS:
		process_sleep_ms((unsigned int)a1);
		return 0;
	case SYS_EXIT:
		process_exit();   /* never returns */
		return 0;
	case SYS_READ_FILE:
		return sys_read_file((const char *)a1, (void *)a2, (unsigned int)a3);
	case SYS_WRITE_FILE:
		return sys_write_file((const char *)a1, (const void *)a2,
				      (unsigned int)a3);
	case SYS_LIST_DIR:
		return sys_list_dir((const char *)a1, (unsigned int)a2,
				    (struct sys_dirent *)a3);
	case SYS_UNLINK:
		return sys_unlink((const char *)a1);
	case SYS_MKDIR:
		return sys_mkdir((const char *)a1);
	case SYS_RMDIR:
		return sys_rmdir((const char *)a1);
	case SYS_LIST_MOUNTS:
		return sys_list_mounts((unsigned int)a1, (struct sys_mount *)a2);
	case SYS_MEMINFO:
		return sys_meminfo((struct sys_meminfo *)a1);
	case SYS_E820:
		return sys_e820((unsigned int)a1, (struct sys_e820 *)a2);
	case SYS_MOUSEINFO:
		return sys_mouseinfo((struct sys_mouseinfo *)a1);
	case SYS_STORAGE_INFO:
		return sys_storage_info((struct sys_storage_info *)a1);
	case SYS_BLKDEV:
		return sys_blkdev((unsigned int)a1, (struct sys_blkdev *)a2);
	case SYS_NET_INFO:
		return sys_net_info((struct sys_netinfo *)a1);
	case SYS_NET_PRINT_INFO:
		return sys_net_print_info();
	case SYS_NET_ACQUIRE:
		return sys_net_acquire();
	case SYS_NET_RELEASE:
		return sys_net_release();
	case SYS_NET_MAC:
		return sys_net_mac((unsigned char *)a1);
	case SYS_NET_GET_IP:
		sys_net_get_ip((unsigned char *)a1);
		return 0;
	case SYS_NET_SET_IP:
		sys_net_set_ip((const unsigned char *)a1);
		return 0;
	case SYS_NET_GET_MASK:
		sys_net_get_mask((unsigned char *)a1);
		return 0;
	case SYS_NET_GET_GW:
		sys_net_get_gw((unsigned char *)a1);
		return 0;
	case SYS_NET_GET_DNS:
		sys_net_get_dns((unsigned char *)a1);
		return 0;
	case SYS_NET_SET_DNS:
		sys_net_set_dns((const unsigned char *)a1);
		return 0;
	case SYS_NET_TX:
		return sys_net_tx((const unsigned char *)a1, (unsigned int)a2);
	case SYS_NET_RX:
		return sys_net_rx((unsigned char *)a1, (unsigned int)a2);
	case SYS_NET_DECODE:
		sys_net_decode((const unsigned char *)a1, (unsigned int)a2);
		return 0;
	case SYS_NET_REPLY:
		return sys_net_reply((const unsigned char *)a1, (unsigned int)a2,
				     (unsigned char *)a3);
	case SYS_NET_LOOPBACK:
		return sys_net_loopback(a1);
	case SYS_NET_REGS:
		return sys_net_regs();
	case SYS_NET_RESET:
		return sys_net_reset();
	case SYS_DHCP_RUN:
		return sys_dhcp_run((unsigned int)a1);
	case SYS_DHCP_INFO:
		return sys_dhcp_info((unsigned char *)a1, (unsigned int *)a2);
	case SYS_DNS_RESOLVE:
		return sys_dns_resolve((const char *)a1, (unsigned char *)a2);
	case SYS_NETLOG:
		return sys_netlog((const char *)a1);
	case SYS_NETLOG_CONF:
		return sys_netlog_conf(a1, (const unsigned char *)a2);
	case SYS_TCP_CONNECT:
		return sys_tcp_connect((const unsigned char *)a1,
				       (const unsigned char *)a2,
				       (unsigned int)a3);
	case SYS_TCP_PEER_MSS:
		return sys_tcp_peer_mss();
	case SYS_TCP_SEND:
		return sys_tcp_send((const void *)a1, (unsigned int)a2);
	case SYS_TCP_RECV:
		return sys_tcp_recv((void *)a1, (unsigned int)a2, (unsigned int)a3);
	case SYS_TCP_CLOSE:
		sys_tcp_close();
		return 0;
	case SYS_NET_NEXT_HOP:
		return sys_net_next_hop((const unsigned char *)a1,
					(unsigned char *)a2);
	case SYS_NET_ARP:
		return sys_net_arp((const unsigned char *)a1, (unsigned char *)a2);
	case SYS_NET_CONFIGURED:
		return sys_net_configured();
	case SYS_GETCWD: {
		const char *cwd = procs[current_pid].cwd;
		char *out = (char *)a1;
		unsigned int i = 0, max = (unsigned int)a2;

		if (!cwd || max == 0)
			return -1;
		for (i = 0; cwd[i] && i + 1 < max; i++)
			out[i] = cwd[i];
		out[i] = '\0';
		return 0;
	}
	case SYS_PROCINFO:
		return process_procinfo((unsigned int)a1,
					(struct sys_procinfo *)a2);
	case SYS_GET_CURSOR:
		return (int)io_get_cursor();
	case SYS_SET_CURSOR:
		io_set_cursor((unsigned int)a1);
		return 0;
	case SYS_GFX_BEGIN:
		return sys_gfx_begin((struct sys_gfx_info *)a1);
	case SYS_GFX_PRESENT:
		return sys_gfx_present();
	case SYS_GFX_END:
		return sys_gfx_end();
	case SYS_KEY_POLL:
		return sys_gfx_key_poll();
	case SYS_RTC_HMS: {
		struct sys_time *out = (struct sys_time *)a1;
		unsigned int h, m, s;

		if (!out)
			return -1;
		rtc_read_hms(&h, &m, &s);
		out->hour = h;
		out->minute = m;
		out->second = s;
		return 0;
	}
	default:
		return -1;
	}
}
