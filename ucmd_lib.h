/* Tiny userland shim for ring-3 commands: syscall wrappers + string output.
 * Freestanding (no libc); include "syscall.h" for the syscall numbers. */
#ifndef UCMD_LIB_H
#define UCMD_LIB_H

#include "syscall.h"
#include "wm_abi.h"

static inline int u_syscall1(int num, int a1)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1)
			  : "memory");
	return ret;
}

static inline int u_syscall0(int num)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num)
			  : "memory");
	return ret;
}

static inline int u_syscall2(int num, int a1, int a2)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1), "c"(a2)
			  : "memory");
	return ret;
}

static inline int u_syscall3(int num, int a1, int a2, int a3)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1), "c"(a2), "d"(a3)
			  : "memory");
	return ret;
}

static inline void u_putc(char c)
{
	char b[2];

	b[0] = c;
	b[1] = '\0';
	u_syscall1(SYS_PUTSTR, (int)b);
}

static inline void u_puts(const char *s)
{
	u_syscall1(SYS_PUTSTR, (int)s);
}

static inline void u_putdec(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;

	*p = '\0';
	if (n == 0) {
		u_putc('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	u_puts(p);
}

static inline void u_puthex(unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[11];
	int i;

	for (i = 0; i < 8; i++)
		buf[i] = hex[(n >> (28 - i * 4)) & 0xF];
	buf[8] = '\0';
	u_puts(buf);
}

/* "a.b.c.d" (strict, 4 parts, 0-255) -> 0, else -1. */
static inline int u_parse_ip(const char *s, unsigned char ip[4])
{
	unsigned int i, val = 0, nparts = 0;
	int have_digit = 0;

	for (i = 0; ; i++) {
		char c = s[i];

		if (c >= '0' && c <= '9') {
			val = val * 10 + (unsigned int)(c - '0');
			if (val > 255)
				return -1;
			have_digit = 1;
		} else if (c == '.' || c == '\0') {
			if (!have_digit || nparts >= 4)
				return -1;
			ip[nparts++] = (unsigned char)val;
			val = 0;
			have_digit = 0;
			if (c == '\0')
				break;
		} else {
			return -1;
		}
	}
	return (nparts == 4) ? 0 : -1;
}

static inline void u_put_ip(const unsigned char ip[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (i)
			u_putc('.');
		u_putdec(ip[i]);
	}
}

static inline void u_put_hex2(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";

	u_putc(hex[(v >> 4) & 0xF]);
	u_putc(hex[v & 0xF]);
}

static inline void u_put_mac(const unsigned char mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		if (i)
			u_putc(':');
		u_put_hex2(mac[i]);
	}
}

static inline int u_getcwd(char *buf, unsigned int max)
{
	return u_syscall2(SYS_GETCWD, (int)buf, (int)max);
}

static inline unsigned int u_ticks_ms(void)
{
	return (unsigned int)u_syscall0(SYS_GET_TICKS_MS);
}

/* Cursor position of this process's terminal screen (linear cell index);
 * used by `top` to repaint its frame in place. */
static inline void u_sleep_ms(unsigned int ms)
{
	u_syscall1(SYS_SLEEP_MS, (int)ms);
}

static inline void u_exit(int code)
{
	u_syscall1(SYS_EXIT, code);
	for (;;)
		;
}

static inline int u_rtc_hms(struct sys_time *t)
{
	return u_syscall1(SYS_RTC_HMS, (int)t);
}

static inline int u_rtc_datetime(char *buf, unsigned int max)
{
	return u_syscall2(SYS_RTC_DATETIME, (int)buf, (int)max);
}

/* Full RTC date/time (year 1970-2069) and its setter.  The setter validates
 * the fields and returns 0 on success. */
static inline int u_rtc_get(struct sys_datetime *t)
{
	return u_syscall1(SYS_RTC_GET, (int)t);
}

static inline int u_rtc_set(const struct sys_datetime *t)
{
	return u_syscall1(SYS_RTC_SET, (int)t);
}

/* Fetch the UTC time from an SNTP server (empty name = pool.ntp.org) and
 * fill `dt`.  Returns SYS_NTP_*; the kernel owns the NIC for the duration. */
static inline int u_ntp_sync(const char *server, unsigned int timeout_ms,
			     struct sys_datetime *dt)
{
	return u_syscall3(SYS_NTP_SYNC, (int)server, (int)timeout_ms, (int)dt);
}

/* Window-manager service (only the kernel-spawned /ramfs/bin/wm may use it). */
static inline int u_wm_attach(struct wm_screen_info *out)
{
	return u_syscall1(SYS_WM_ATTACH, (int)out);
}

static inline int u_wm_info(unsigned int index, struct wm_window_info *out)
{
	return u_syscall2(SYS_WM_INFO, (int)index, (int)out);
}

static inline int u_wm_map(unsigned int index, struct wm_map_info *out)
{
	return u_syscall2(SYS_WM_MAP, (int)index, (int)out);
}

static inline int u_wm_ack(unsigned int index, unsigned int seq)
{
	return u_syscall2(SYS_WM_ACK, (int)index, (int)seq);
}

static inline int u_wm_focus(unsigned int index)
{
	return u_syscall1(SYS_WM_FOCUS, (int)index);
}

static inline int u_wm_set_geom(unsigned int index, int x, int y)
{
	struct wm_geom g;

	g.x = x;
	g.y = y;
	return u_syscall2(SYS_WM_SET_GEOM, (int)index, (int)&g);
}

static inline int u_wm_create_term(void)
{
	return u_syscall0(SYS_WM_CREATE_TERM);
}

static inline int u_wm_close(unsigned int index)
{
	return u_syscall1(SYS_WM_CLOSE, (int)index);
}

static inline int u_wm_poll(struct wm_status *out)
{
	return u_syscall1(SYS_WM_POLL, (int)out);
}

static inline int u_wm_begin(void)
{
	return u_syscall0(SYS_WM_BEGIN);
}

static inline int u_wm_present(struct wm_present_info *in)
{
	return u_syscall1(SYS_WM_PRESENT, (int)in);
}

static inline int u_wm_sync(void)
{
	return u_syscall0(SYS_WM_SYNC);
}

/* Generic canvas window (gfx commands own a window instead of replacing a
 * terminal's content). */
static inline int u_win_create(unsigned int w, unsigned int h, const char *title,
			       struct win_info *out)
{
	struct win_create in;
	unsigned int i = 0;

	in.w = w;
	in.h = h;
	while (title && title[i] && i < sizeof(in.title) - 1) {
		in.title[i] = title[i];
		i++;
	}
	in.title[i] = '\0';
	return u_syscall2(SYS_WIN_CREATE, (int)&in, (int)out);
}

static inline int u_win_present(void)
{
	return u_syscall1(SYS_WIN_PRESENT, 0);
}

/* Publish only the content-relative rectangle that changed. */
static inline int u_win_present_rect(int x, int y, unsigned int w,
				     unsigned int h)
{
	struct wm_win_present in;

	in.x = (unsigned int)x;
	in.y = (unsigned int)y;
	in.w = w;
	in.h = h;
	return u_syscall1(SYS_WIN_PRESENT, (int)&in);
}

static inline int u_win_close(void)
{
	return u_syscall0(SYS_WIN_CLOSE);
}

static inline int u_win_info(struct win_info *out)
{
	return u_syscall1(SYS_WIN_INFO, (int)out);
}

static inline int u_win_ack_resize(void)
{
	return u_syscall0(SYS_WIN_ACK_RESIZE);
}

static inline int u_win_pointer(struct win_pointer *out)
{
	return u_syscall1(SYS_WIN_POINTER, (int)out);
}

static inline int u_win_event(struct win_event *out)
{
	return u_syscall1(SYS_WIN_EVENT, (int)out);
}

static inline int u_wm_resize(unsigned int index, unsigned int cw,
			      unsigned int ch)
{
	struct wm_size s;

	s.w = cw;
	s.h = ch;
	return u_syscall2(SYS_WM_RESIZE, (int)index, (int)&s);
}

static inline int u_win_key_wait(unsigned int timeout_ms)
{
	return u_syscall1(SYS_WIN_KEY_WAIT, (int)timeout_ms);
}

static inline int u_tty_create(void)
{
	return u_syscall0(SYS_TTY_CREATE);
}

static inline int u_tty_read(int id, char *buf, unsigned int max)
{
	return u_syscall3(SYS_TTY_READ, id, (int)buf, (int)max);
}

static inline int u_spawn(const char *path, const char *arg, int tty,
			  const char *cwd)
{
	struct sys_spawn s;

	s.path = path;
	s.arg = arg;
	s.cwd = cwd;
	s.tty = (unsigned int)tty;
	return u_syscall1(SYS_SPAWN, (int)&s);
}

static inline int u_spawn_kcmd(const char *name, const char *arg, int tty)
{
	return u_syscall3(SYS_SPAWN_KCMD, (int)name, (int)arg, tty);
}

static inline int u_alive(unsigned int pid)
{
	return u_syscall1(SYS_ALIVE, (int)pid);
}

static inline int u_kill(unsigned int pid)
{
	return u_syscall1(SYS_KILL, (int)pid);
}

static inline int u_chdir(const char *path)
{
	return u_syscall1(SYS_CHDIR, (int)path);
}

static inline int u_key_poll(void)
{
	return u_syscall0(SYS_KEY_POLL);
}

static inline int u_schedinfo(struct sys_schedinfo *out)
{
	return u_syscall1(SYS_SCHEDINFO, (int)out);
}

static inline int u_cpu_temp(struct sys_cputemp *out)
{
	return u_syscall1(SYS_CPU_TEMP, (int)out);
}

static inline int u_sensors(struct sys_sensors *out)
{
	return u_syscall1(SYS_SENSORS, (int)out);
}

static inline int u_tpec_sensors(struct sys_tpec_sensors *out)
{
	return u_syscall1(SYS_TPEC_SENSORS, (int)out);
}

static inline int u_power_info(struct sys_powerinfo *out, unsigned int flags)
{
	return u_syscall2(SYS_POWER_INFO, (int)out, (int)flags);
}

/* Path metadata (size, directory flag, read-only mount). */
static inline int u_stat(const char *path, struct sys_stat *out)
{
	return u_syscall2(SYS_FS_STAT, (int)path, (int)out);
}

/* TCP server (the caller owns the NIC). */
static inline int u_tcp_listen(unsigned int port)
{
	return u_syscall1(SYS_TCP_LISTEN, (int)port);
}

static inline int u_tcp_accept(void)
{
	return u_syscall0(SYS_TCP_ACCEPT);
}

static inline int u_tcp_srv_recv(void *buf, unsigned int max,
				 unsigned int timeout_ms)
{
	return u_syscall3(SYS_TCP_SRV_RECV, (int)buf, (int)max,
			  (int)timeout_ms);
}

static inline int u_tcp_srv_send(const void *buf, unsigned int len)
{
	return u_syscall2(SYS_TCP_SRV_SEND, (int)buf, (int)len);
}

static inline void u_tcp_srv_close(void)
{
	u_syscall0(SYS_TCP_SRV_CLOSE);
}

static inline int u_tcp_peer(unsigned char ip[4], unsigned int *port)
{
	return u_syscall2(SYS_TCP_PEER, (int)ip, (int)port);
}

/* Raw FAT 8.3 field -> display name ("FOO     TXT" -> "FOO.TXT"). */
static inline void u_fat_name(const char *raw, char *out, unsigned int cap)
{
	unsigned int n = 0, i, ext = 8;

	while (ext < 11 && raw[ext] == ' ')
		ext++;
	for (i = 0; i < 8 && raw[i] && raw[i] != ' ' && n + 1 < cap; i++)
		out[n++] = raw[i];
	if (ext < 11 && raw[ext]) {
		if (n + 1 < cap)
			out[n++] = '.';
		for (i = ext; i < 11 && raw[i] && raw[i] != ' ' &&
		     n + 1 < cap; i++)
			out[n++] = raw[i];
	}
	out[n] = '\0';
}

#endif
