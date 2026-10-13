/* Window manager: fixed-size draggable terminal windows + a system bar. */
#ifndef WM_H
#define WM_H

#include "terminal.h"

#define MAX_WINDOWS 4

/* System bar (banner + "+ New" button + clock + rainbow) at the top. */
#define WM_BAR_H 25
#define WM_BTN_X 132
#define WM_BTN_Y 3
#define WM_BTN_W 56
#define WM_BTN_H 14

/* Window chrome. */
#define WM_TITLE_H 16
#define WM_CLOSE_W 14

struct window {
	int used;
	unsigned int x, y;       /* pixel origin (title bar top-left) */
	unsigned int pw, ph;     /* pixel size */
	char title[16];
	int focused;
	unsigned int pid;        /* owning terminal process pid (0 = none) */
	struct term *term;
};

void wm_run(void);
int  wm_spawn_terminal(void);
struct window *wm_focused_window(void);
int  wm_is_focused(const struct window *w);
void wm_request_redraw(void);

/* The terminal whose screen is `screen`, or NULL (the sys_gfx service uses the
 * inherited screen pointer to find a command's window). */
struct term *wm_term_for_screen(struct text_screen *screen);

/* Pixel size of a window's content area (its canvas size).  Returns 0/-1. */
int wm_content_size(struct term *t, unsigned int *w, unsigned int *h);

#endif
