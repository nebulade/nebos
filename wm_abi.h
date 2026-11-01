/* Shared window-manager ABI: kernel <-> ring-3 WM.
 * Only plain types and syscall numbers live here; the kernel window state and
 * all font/pixel decisions stay elsewhere. */
#ifndef WM_ABI_H
#define WM_ABI_H

#define WM_MAX_WINDOWS 4

/* Shared chrome layout (the WM draws it, the kernel only stores geometry). */
#define WM_BAR_H   30
#define WM_BTN_X   132
#define WM_BTN_Y   5
#define WM_BTN_W   56
#define WM_BTN_H   20
#define WM_TITLE_H 22
#define WM_CLOSE_W 16

/* Fixed user virtual addresses for the WM's mappings.  The shadow is one
 * mapping; each window gets a private content slot (large enough for the
 * current canvas and future bigger ones). */
#define WM_SHADOW_VA        0x58000000u
#define WM_CONTENT_VA_BASE  0x59000000u
#define WM_CONTENT_SLOT     0x00200000u   /* 2 MB per window */

/* Window content kind. */
#define WM_KIND_NONE 0
#define WM_KIND_TEXT 1
#define WM_KIND_GFX  2

struct wm_screen_info {
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
	unsigned int bpp;
	unsigned int bytes;
	void *fb;
};

struct wm_mouse_info {
	unsigned int x;
	unsigned int y;
	unsigned int buttons;      /* bit0 left, bit1 right, bit2 middle */
};

struct wm_status {
	struct wm_mouse_info mouse;
	unsigned int full_redraw;
	unsigned int kexec_busy;
	unsigned int kexec_progress;
	unsigned int kexec_total;
};

struct wm_window_info {
	unsigned int used;
	unsigned int kind;         /* WM_KIND_* */
	unsigned int focused;
	unsigned int pid;
	int x, y;
	unsigned int w, h;         /* outer window rectangle */
	unsigned int content_w, content_h;
	unsigned int cols, rows;   /* text cells (0 for gfx) */
	unsigned int cursor;
	unsigned int cursor_visible;
	unsigned int dirty;
	unsigned int dirty_seq;
	unsigned int dmg_col0, dmg_row0, dmg_cols, dmg_rows;
	unsigned int content_gen;
	char title[16];
};

struct wm_map_info {
	void *va;
	unsigned int kind;
	unsigned int w, h;
	unsigned int stride;       /* bytes per row for gfx, cells*2 for text */
	unsigned int generation;
};

struct wm_rect {
	int x, y;
	unsigned int w, h;
};

struct wm_present_info {
	unsigned int count;        /* 0..4 */
	unsigned int immediate;    /* 1: flush now (drag); 0: budgeted */
	struct wm_rect rect[4];
};

struct wm_geom {
	int x, y;
};

struct wm_size {
	unsigned int w, h;
};

/* Generic canvas-window client API (gfx commands own their own window). */
struct win_create {
	unsigned int w, h;
	char title[16];
};

struct win_info {
	void *fb;                  /* mapped RGB565 back buffer */
	unsigned int width, height;
	unsigned int id;
};

#endif
