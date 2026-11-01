/* Syscall dispatcher: services int 0x80 requests from ring-3. */

#include "syscall.h"
#include "io.h"
#include "paging.h"
#include "process.h"
#include "rtc.h"
#include "serial.h"
#include "sys_fs.h"
#include "sys_gfx.h"
#include "sys_info.h"
#include "sys_net.h"
#include "sys_wm.h"
#include "terminal.h"
#include "timer.h"
#include "tty.h"
#include "vfs.h"

static int copy_user_string(char *dst, unsigned int max, const char *src)
{
	unsigned int i;

	if (!src || !max ||
	    !paging_user_range_ok(current_proc()->cr3, (unsigned int)src,
				  max, 0))
		return -1;
	for (i = 0; i + 1 < max && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	return 0;
}

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
		const char *cwd = current_proc()->cwd;
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
	case SYS_RTC_DATETIME: {
		char buf[20];
		char *out = (char *)a1;
		unsigned int i;

		if ((unsigned int)a2 < sizeof(buf) ||
		    !paging_user_range_ok(current_proc()->cr3,
					  (unsigned int)out, sizeof(buf), 1))
			return -1;
		rtc_read_datetime(buf);
		for (i = 0; i < sizeof(buf); i++)
			out[i] = buf[i];
		return 0;
	}
	case SYS_WM_ATTACH:
		return sys_wm_attach((struct wm_screen_info *)a1);
	case SYS_WM_INFO:
		return sys_wm_info((unsigned int)a1,
				   (struct wm_window_info *)a2);
	case SYS_WM_MAP:
		return sys_wm_map((unsigned int)a1,
				  (struct wm_map_info *)a2);
	case SYS_WM_ACK:
		return sys_wm_ack((unsigned int)a1, (unsigned int)a2);
	case SYS_WM_FOCUS:
		return sys_wm_focus((unsigned int)a1);
	case SYS_WM_SET_GEOM:
		return sys_wm_set_geom((unsigned int)a1,
				       (const struct wm_geom *)a2);
	case SYS_WM_CREATE_TERM:
		return sys_wm_create_term();
	case SYS_WM_CLOSE:
		return sys_wm_close((unsigned int)a1);
	case SYS_WM_POLL:
		return sys_wm_poll((struct wm_status *)a1);
	case SYS_WM_BEGIN:
		return sys_wm_begin();
	case SYS_WM_PRESENT:
		return sys_wm_present((const struct wm_present_info *)a1);
	case SYS_WM_SYNC:
		return sys_wm_sync();
	case SYS_WIN_CREATE:
		return sys_win_create((const struct win_create *)a1,
				      (struct win_info *)a2);
	case SYS_WIN_PRESENT:
		return sys_win_present();
	case SYS_WIN_CLOSE:
		return sys_win_close();
	case SYS_WM_RESIZE:
		return sys_wm_resize((unsigned int)a1,
				     (const struct wm_size *)a2);
	case SYS_WIN_INFO:
		return sys_win_info((struct win_info *)a1);
	case SYS_WIN_ACK_RESIZE:
		return sys_win_ack_resize();
	case SYS_WIN_KEY_WAIT:
		return sys_win_key_wait((unsigned int)a1);
	case SYS_SCHEDINFO:
		if (!paging_user_range_ok(current_proc()->cr3, (unsigned int)a1,
					  sizeof(struct sys_schedinfo), 1))
			return -1;
		return sched_snapshot((struct sys_schedinfo *)a1);
	case SYS_TTY_CREATE:
		return tty_create();
	case SYS_TTY_READ: {
		int id = a1;
		char *buf = (char *)a2;
		unsigned int max = (unsigned int)a3;

		if (!tty_valid(id) || !buf || !max ||
		    !paging_user_range_ok(current_proc()->cr3,
					  (unsigned int)buf, max, 1))
			return -1;
		return tty_read(id, buf, max);
	}
	case SYS_SPAWN: {
		struct sys_spawn in;
		struct vfs_file f;
		struct vfs_mount *m = 0;
		char path[VFS_PATH_MAX], arg[VFS_PATH_MAX], cwd[VFS_PATH_MAX];
		const char *name;
		int pid;

		if (!paging_user_range_ok(current_proc()->cr3,
					  (unsigned int)a1, sizeof(in), 0))
			return -1;
		{
			const char *p = (const char *)a1;
			unsigned int i;
			unsigned char *d = (unsigned char *)&in;

			for (i = 0; i < sizeof(in); i++)
				d[i] = (unsigned char)p[i];
		}
		if (in.tty && !tty_valid((int)in.tty))
			return -1;
		if (copy_user_string(path, sizeof(path), in.path) != 0 ||
		    copy_user_string(arg, sizeof(arg), in.arg) != 0 ||
		    copy_user_string(cwd, sizeof(cwd), in.cwd) != 0)
			return -1;
		if (vfs_load_file(path, &f, &m) != 0)
			return -1;
		name = path;
		{
			const char *q = path;
			while (*q) {
				if (*q == '/')
					name = q + 1;
				q++;
			}
		}
		pid = process_create_user_tty(name, f.data, f.size, arg, cwd,
					      in.tty);
		vfs_release_file(m, &f);
		return pid;
	}
	case SYS_SPAWN_KCMD: {
		char name[16], arg[VFS_PATH_MAX];

		if (copy_user_string(name, sizeof(name), (const char *)a1) != 0 ||
		    copy_user_string(arg, sizeof(arg), (const char *)a2) != 0)
			return -1;
		if (a3 && !tty_valid(a3))
			return -1;
		return term_kcmd_spawn(name, arg, (unsigned int)a3);
	}
	case SYS_ALIVE:
		return process_is_alive((unsigned int)a1);
	case SYS_KILL:
		return process_kill((unsigned int)a1);
	case SYS_CHDIR: {
		char arg[VFS_PATH_MAX], abs[VFS_PATH_MAX];
		unsigned int size;
		int is_dir;

		if (copy_user_string(arg, sizeof(arg), (const char *)a1) != 0)
			return -1;
		if (vfs_join_path(current_proc()->cwd, arg[0] ? arg : ".",
				  abs, sizeof(abs)) != 0)
			return -1;
		if (vfs_stat(abs, &size, &is_dir) != 0 || !is_dir)
			return -1;
		{
			unsigned int i;
			for (i = 0; abs[i] && i + 1 < sizeof(current_proc()->cwd); i++)
				current_proc()->cwd[i] = abs[i];
			current_proc()->cwd[i] = '\0';
		}
		return 0;
	}
	default:
		return -1;
	}
}
