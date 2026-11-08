/* Window manager: fixed-size draggable terminal windows + a system bar. */
#ifndef WM_H
#define WM_H

#include "gfx.h"
#include "sched.h"
#include "wm_abi.h"

#define MAX_WINDOWS WM_MAX_WINDOWS

/* Hard sanity bound for a canvas window (stops size arithmetic wrapping);
 * the framebuffer check below it is the real limit.  The old 1024 px cap
 * left a 1280x800 panel unable to enlarge a window past 1024. */
#define WM_WIN_MAX_W 2048
#define WM_WIN_MAX_H 2048

struct window {
	int used;
	unsigned int x, y;       /* pixel origin (title bar top-left) */
	unsigned int pw, ph;     /* pixel size */
	char title[16];
	int focused;
	unsigned int pid;        /* owning process pid (0 = none) */
	/* Every window is a ring-3 canvas client. */
	int canvas_window;
	struct gfx_canvas canvas;
	unsigned short *back;
	unsigned int back_pages;
	unsigned short *pending_back;   /* new canvas waiting for the app ACK */
	unsigned int pending_pages;
	int resize_pending;
	unsigned int owner_cr3;
	int keys[16];
	unsigned int key_head, key_tail;
	struct wait_queue key_wq;
	/* Pointer press/release edges for a generic canvas window. */
	struct win_event ptr_events[8];
	unsigned int ptr_head, ptr_tail;
	/* Content-relative damage union since the WM last ACKed a present. */
	unsigned int canvas_dmg_x, canvas_dmg_y;
	unsigned int canvas_dmg_w, canvas_dmg_h;
	/* Ring-3 WM content mapping/bookkeeping. */
	unsigned int content_gen; /* bumped when the content kind/session changes */
	unsigned int mapped_kind;
	unsigned int mapped_gen;
	unsigned int mapped_pages;
	const void *mapped_src;
};

/* Idle-loop services: these stay in the kernel even while the WM process is
 * stopped, so keyboard routing and kexec recovery keep working. */
void wm_pump_keyboard(void);
void wm_handle_kexec(void);
int  wm_spawn_terminal(void);

/* Kernel window server / ring-3 WM ABI (see wm_abi.h). */
void wm_server_init(void);
void wm_server_start(void);
void wm_server_supervise(void);
int  wm_server_is_wm(void);
int  wm_server_frame_active(void);
int  wm_server_can_reap(void);
int  wm_server_attach(struct wm_screen_info *out);
int  wm_server_info(unsigned int index, struct wm_window_info *out);
int  wm_server_map(unsigned int index, struct wm_map_info *out);
int  wm_server_ack(unsigned int index, unsigned int seq);
int  wm_server_focus(unsigned int index);
int  wm_server_set_geom(unsigned int index, int x, int y);
int  wm_server_create_term(void);
int  wm_server_close(unsigned int index);
int  wm_server_poll(struct wm_status *out);
int  wm_server_begin(void);
int  wm_server_present(const struct wm_present_info *in);
int  wm_server_sync(void);
int  wm_server_win_create(const struct win_create *in, struct win_info *out);
int  wm_server_win_present(const struct wm_win_present *in);
int  wm_server_win_close(void);
int  wm_server_win_info(struct win_info *out);
int  wm_server_win_ack_resize(void);
int  wm_server_resize(unsigned int index, unsigned int cw, unsigned int ch);
int  wm_server_key_poll(void);
int  wm_server_key_wait(unsigned int timeout_ms);
int  wm_server_win_pointer(struct win_pointer *out);
int  wm_server_win_event(struct win_event *out);
void wm_server_boot_ready(void);
struct window *wm_focused_window(void);
int  wm_is_focused(const struct window *w);
void wm_request_redraw(void);

#endif
