/* Syscall wrappers for the ring-3 window manager.  User pointers are checked
 * against the caller's page tables and copied before the kernel touches them;
 * a buggy WM returns -1 instead of faulting in ring 0. */

#include "sys_wm.h"
#include "paging.h"
#include "process.h"
#include "wm.h"

static int user_ok(const void *p, unsigned int size, int write)
{
	return paging_user_range_ok(current_proc()->cr3,
				    (unsigned int)p, size, write);
}

static void copy_to_user(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--)
		*d++ = *s++;
}

static void copy_from_user(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--)
		*d++ = *s++;
}

static void zero_struct(void *p, unsigned int n)
{
	unsigned char *d = (unsigned char *)p;
	while (n--)
		*d++ = 0;
}

int sys_wm_attach(struct wm_screen_info *out)
{
	struct wm_screen_info info;
	int r;

	if (!user_ok(out, sizeof(info), 1))
		return -1;
	zero_struct(&info, sizeof(info));
	r = wm_server_attach(&info);
	if (r == 0)
		copy_to_user(out, &info, sizeof(info));
	return r;
}

int sys_wm_info(unsigned int index, struct wm_window_info *out)
{
	struct wm_window_info info;
	int r;

	if (!user_ok(out, sizeof(info), 1))
		return -1;
	zero_struct(&info, sizeof(info));
	r = wm_server_info(index, &info);
	if (r >= 0)
		copy_to_user(out, &info, sizeof(info));
	return r;
}

int sys_wm_map(unsigned int index, struct wm_map_info *out)
{
	struct wm_map_info info;
	int r;

	if (!user_ok(out, sizeof(info), 1))
		return -1;
	zero_struct(&info, sizeof(info));
	r = wm_server_map(index, &info);
	if (r == 0)
		copy_to_user(out, &info, sizeof(info));
	return r;
}

int sys_wm_ack(unsigned int index, unsigned int seq)
{
	return wm_server_ack(index, seq);
}

int sys_wm_focus(unsigned int index)
{
	return wm_server_focus(index);
}

int sys_wm_set_geom(unsigned int index, const struct wm_geom *in)
{
	struct wm_geom g;

	if (!user_ok(in, sizeof(g), 0))
		return -1;
	copy_from_user(&g, in, sizeof(g));
	return wm_server_set_geom(index, g.x, g.y);
}

int sys_wm_create_term(void)
{
	return wm_server_create_term();
}

int sys_wm_close(unsigned int index)
{
	return wm_server_close(index);
}

int sys_wm_poll(struct wm_status *out)
{
	struct wm_status st;
	int r;

	if (!user_ok(out, sizeof(st), 1))
		return -1;
	zero_struct(&st, sizeof(st));
	r = wm_server_poll(&st);
	if (r == 0)
		copy_to_user(out, &st, sizeof(st));
	return r;
}

int sys_wm_begin(void)
{
	return wm_server_begin();
}

int sys_wm_present(const struct wm_present_info *in)
{
	struct wm_present_info p;

	if (!user_ok(in, sizeof(p), 0))
		return -1;
	copy_from_user(&p, in, sizeof(p));
	return wm_server_present(&p);
}

int sys_wm_key_poll(void)
{
	return wm_server_key_poll();
}

int sys_wm_sync(void)
{
	return wm_server_sync();
}

int sys_win_create(const struct win_create *in, struct win_info *out)
{
	struct win_create create;
	struct win_info info;
	int r;

	if (!user_ok(in, sizeof(create), 0) ||
	    !user_ok(out, sizeof(info), 1))
		return -1;
	copy_from_user(&create, in, sizeof(create));
	zero_struct(&info, sizeof(info));
	r = wm_server_win_create(&create, &info);
	if (r == 0)
		copy_to_user(out, &info, sizeof(info));
	return r;
}

int sys_win_present(const struct wm_win_present *in)
{
	struct wm_win_present p;

	if (!in)
		return wm_server_win_present(0);
	if (!user_ok(in, sizeof(p), 0))
		return -1;
	copy_from_user(&p, in, sizeof(p));
	return wm_server_win_present(&p);
}

int sys_win_close(void)
{
	return wm_server_win_close();
}

int sys_win_info(struct win_info *out)
{
	struct win_info info;
	int r;

	if (!user_ok(out, sizeof(info), 1))
		return -1;
	zero_struct(&info, sizeof(info));
	r = wm_server_win_info(&info);
	if (r == 0)
		copy_to_user(out, &info, sizeof(info));
	return r;
}

int sys_win_ack_resize(void)
{
	return wm_server_win_ack_resize();
}

int sys_win_pointer(struct win_pointer *out)
{
	struct win_pointer p;
	int r;

	if (!user_ok(out, sizeof(p), 1))
		return -1;
	zero_struct(&p, sizeof(p));
	r = wm_server_win_pointer(&p);
	if (r == 0)
		copy_to_user(out, &p, sizeof(p));
	return r;
}

int sys_win_event(struct win_event *out)
{
	struct win_event e;
	int r;

	if (!user_ok(out, sizeof(e), 1))
		return -1;
	zero_struct(&e, sizeof(e));
	r = wm_server_win_event(&e);
	if (r == 1)
		copy_to_user(out, &e, sizeof(e));
	return r;
}

int sys_wm_resize(unsigned int index, const struct wm_size *in)
{
	struct wm_size s;

	if (!user_ok(in, sizeof(s), 0))
		return -1;
	copy_from_user(&s, in, sizeof(s));
	return wm_server_resize(index, s.w, s.h);
}

int sys_win_key_wait(unsigned int timeout_ms)
{
	return wm_server_key_wait(timeout_ms);
}
