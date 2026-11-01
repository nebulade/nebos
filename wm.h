/* Window manager: fixed-size draggable terminal windows + a system bar. */
#ifndef WM_H
#define WM_H

#include "terminal.h"
#include "gfx.h"
#include "wm_abi.h"

#define MAX_WINDOWS WM_MAX_WINDOWS

struct window {
	int used;
	unsigned int x, y;       /* pixel origin (title bar top-left) */
	unsigned int pw, ph;     /* pixel size */
	char title[16];
	int focused;
	unsigned int pid;        /* owning terminal process pid (0 = none) */
	struct term *term;
	/* Generic canvas window (gfx commands): no terminal, own front/back. */
	int canvas_window;
	struct gfx_canvas canvas;
	unsigned short *back;
	unsigned int back_pages;
	unsigned int owner_cr3;
	int keys[16];
	unsigned int key_head, key_tail;
	struct wait_queue key_wq;
	/* Ring-3 WM content mapping/bookkeeping. */
	unsigned int content_gen; /* bumped when the content kind/session changes */
	unsigned int mapped_kind;
	unsigned int mapped_gen;
	unsigned int mapped_pages;
	const void *mapped_src;
};

void wm_run(void);
/* Idle-loop services: these stay in the kernel even while the WM process is
 * stopped, so keyboard routing, cursor blink and kexec recovery keep working. */
void wm_pump_keyboard(void);
void wm_blink_cursor(void);
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
int  wm_server_win_present(void);
int  wm_server_win_close(void);
int  wm_server_key_poll(void);
int  wm_server_key_wait(unsigned int timeout_ms);
void wm_content_changed(struct text_screen *screen);
void wm_server_unmap_screen(struct text_screen *screen);
void wm_server_unmap_canvas(struct text_screen *screen, const void *canvas);
struct window *wm_focused_window(void);
int  wm_is_focused(const struct window *w);
void wm_request_redraw(void);

/* The terminal whose screen is `screen`, or NULL (the sys_gfx service uses the
 * inherited screen pointer to find a command's window). */
struct term *wm_term_for_screen(struct text_screen *screen);

/* Pixel size of a window's content area (its canvas size).  Returns 0/-1. */
int wm_content_size(struct term *t, unsigned int *w, unsigned int *h);

#endif
