/* Window manager: fixed-size draggable terminal windows + a system bar.
   Slot 0 runs wm_run(); each terminal window is a separate process. */

#include "wm.h"
#include "fbtext.h"
#include "idle.h"
#include "io.h"
#include "kexec.h"
#include "mouse.h"
#include "process.h"
#include "rtc.h"
#include "timer.h"
#include "version.h"
#include "vga.h"

static struct window windows[MAX_WINDOWS];
static struct term terms[MAX_WINDOWS];
static unsigned int term_counter;
static int wm_full_redraw;

/* Mouse drag state. */
static int drag_active;
static int drag_window;      /* index into windows[] */
static int drag_dx, drag_dy; /* mouse offset from window origin */
static unsigned int prev_buttons;

void wm_request_redraw(void)
{
	wm_full_redraw = 1;
}

struct window *wm_focused_window(void)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused)
			return &windows[i];
	return (void *)0;
}

int wm_is_focused(const struct window *w)
{
	return w->focused;
}

static int wm_point_in_rect(unsigned int px, unsigned int py,
	unsigned int rx, unsigned int ry, unsigned int rw, unsigned int rh)
{
	return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void wm_make_title(struct window *w, unsigned int n)
{
	char num[4];
	unsigned int i = 0, k = 0;
	w->title[k++] = 't'; w->title[k++] = 'e'; w->title[k++] = 'r';
	w->title[k++] = 'm'; w->title[k++] = ' ';
	do {
		num[i++] = (char)('0' + (n % 10));
		n /= 10;
	} while (n && i < 3);
	while (i)
		w->title[k++] = num[--i];
	w->title[k] = '\0';
}

int wm_spawn_terminal(void)
{
	unsigned int i, j, fbw = vga_fb_width();
	struct window *w;
	struct term *t;
	int pid;
	unsigned int n;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (!windows[i].used)
			break;
	if (i == MAX_WINDOWS)
		return -1;

	w = &windows[i];
	t = &terms[i];

	screen_init(&t->screen);
	t->screen.cursor_visible = 1;
	t->key_head = t->key_tail = 0;
	t->history_count = 0;
	t->history_next = 0;

	w->used = 1;
	w->term = t;
	w->pid = 0;
	w->focused = 0;
	w->pw = SCREEN_COLS * 8 + 2;
	w->ph = WM_TITLE_H + SCREEN_ROWS * 10 + 2;

	/* Cascade position, clamped to the desktop area. */
	n = term_counter % 5;
	w->x = 24 + n * 32;
	w->y = WM_BAR_H + 12 + n * 28;
	if (w->x + w->pw > fbw)
		w->x = (fbw > w->pw) ? fbw - w->pw : 0;
	if (w->y + w->ph > vga_fb_height())
		w->y = WM_BAR_H + 4;

	term_counter++;
	wm_make_title(w, term_counter);

	pid = process_create_ptr("term", term_entry, t);
	if (pid < 0) {
		w->used = 0;
		return -1;
	}
	w->pid = (unsigned int)pid;

	for (j = 0; j < MAX_WINDOWS; j++)
		if (windows[j].used)
			windows[j].focused = (j == i);

	wm_request_redraw();
	return pid;
}

static void wm_focus_index(unsigned int idx)
{
	unsigned int i;
	if (windows[idx].used && windows[idx].focused)
		return;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used)
			windows[i].focused = (i == idx);
	wm_request_redraw();
}

static void wm_focus_top(void)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used) {
			wm_focus_index(i);
			return;
		}
}

static void wm_close_window(unsigned int idx)
{
	struct window *w = &windows[idx];
	if (w->pid)
		process_kill(w->pid);
	w->used = 0;
	w->pid = 0;
	w->focused = 0;
	wm_focus_top();
	wm_request_redraw();
}

/* Topmost window containing (px,py), or -1. */
static int wm_window_at(unsigned int px, unsigned int py)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused &&
		    wm_point_in_rect(px, py, windows[i].x, windows[i].y,
		    	windows[i].pw, windows[i].ph))
			return (int)i;
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && !windows[i].focused &&
		    wm_point_in_rect(px, py, windows[i].x, windows[i].y,
		    	windows[i].pw, windows[i].ph))
			return (int)i;
	return -1;
}

static void wm_clamp_window(struct window *w, int nx, int ny)
{
	unsigned int fbw = vga_fb_width();
	unsigned int fbh = vga_fb_height();
	int maxx = (int)fbw - (int)w->pw;
	int maxy = (int)fbh - (int)w->ph;
	if (maxx < 0)
		maxx = 0;
	if (maxy < (int)WM_BAR_H)
		maxy = (int)WM_BAR_H;
	if (nx < 0)
		nx = 0;
	else if (nx > maxx)
		nx = maxx;
	if (ny < (int)WM_BAR_H)
		ny = (int)WM_BAR_H;
	else if (ny > maxy)
		ny = maxy;
	w->x = (unsigned int)nx;
	w->y = (unsigned int)ny;
}

static void wm_handle_mouse(void)
{
	unsigned int buttons = mouse_buttons_state();
	unsigned int mx = mouse_get_x();
	unsigned int my = mouse_get_y();
	int left_down = (buttons & 1) != 0;
	int left_was_down = (prev_buttons & 1) != 0;

	if (left_down && !left_was_down) {
		/* Press. */
		if (wm_point_in_rect(mx, my, WM_BTN_X, WM_BTN_Y, WM_BTN_W, WM_BTN_H)) {
			wm_spawn_terminal();
		} else {
			int idx = wm_window_at(mx, my);
			if (idx >= 0) {
				struct window *w = &windows[idx];
				wm_focus_index((unsigned int)idx);
				/* Close button? */
				if (wm_point_in_rect(mx, my,
				    w->x + w->pw - 1 - WM_CLOSE_W, w->y + 1,
				    WM_CLOSE_W, WM_TITLE_H)) {
					wm_close_window((unsigned int)idx);
				} else if (my < w->y + WM_TITLE_H) {
					/* Title bar: start drag. */
					drag_active = 1;
					drag_window = idx;
					drag_dx = (int)mx - (int)w->x;
					drag_dy = (int)my - (int)w->y;
				}
			}
		}
	} else if (left_down && left_was_down && drag_active) {
		struct window *w = &windows[drag_window];
		if (w->used) {
			wm_clamp_window(w, (int)mx - drag_dx, (int)my - drag_dy);
			wm_request_redraw();
		} else {
			drag_active = 0;
		}
	} else if (!left_down && left_was_down) {
		drag_active = 0;
	}

	prev_buttons = buttons;
}

static void wm_handle_keyboard(void)
{
	struct window *fw = wm_focused_window();
	int n;
	if (!fw || !fw->term)
		return;
	for (n = 0; n < 8; n++) {
		int k = io_poll_key();
		if (!k)
			break;
		term_key_push(fw->term, k);
	}
}

static void wm_draw_bar(void)
{
	unsigned int fbw = vga_fb_width();
	char dt[20];

	/* Banner (light green). */
	fbtext_draw_string_xy(4, 5, OS_NAME " v" OS_VERSION, 85, 255, 85, 0);

	/* "+ New" button: purple box, white text. */
	vga_fill_rect(WM_BTN_X, WM_BTN_Y, WM_BTN_W, WM_BTN_H, 150, 60, 210);
	fbtext_draw_string_xy(WM_BTN_X + 4, WM_BTN_Y + 3, "+ New", 255, 255, 255, 1);

	if (kexec_busy) {
		/* Kexec progress, in place of the clock. */
		static const char msg[] = "loading new kernel over serial";
		char buf[48];
		unsigned int n = 0, dots, i;
		while (msg[n])
			buf[n] = msg[n], n++;
		dots = (kexec_total > 0) ? (kexec_progress * 12 / kexec_total) : 0;
		for (i = 0; i < 12; i++)
			buf[n + i] = (i < dots) ? '.' : ' ';
		n += 12;
		buf[n] = '\0';
		fbtext_draw_string_xy(fbw - n * 8 - 4, 5, buf, 255, 200, 100, 0);
	} else {
		/* Live clock (light green, right-aligned). */
		rtc_read_datetime(dt);
		fbtext_draw_string_xy(fbw - 19 * 8 - 4, 5, dt, 85, 255, 85, 0);
	}

	/* Rainbow separator. */
	fbtext_draw_rainbow_separator();
}

static void wm_draw_window(struct window *w)
{
	struct text_screen *s = &w->term->screen;
	unsigned int cx = w->x + 1;
	unsigned int cy = w->y + 1 + WM_TITLE_H;

	/* Border. */
	vga_fill_rect(w->x, w->y, w->pw, w->ph, 200, 200, 200);

	/* Title bar. */
	if (w->focused)
		vga_fill_rect(cx, w->y + 1, w->pw - 2, WM_TITLE_H, 70, 90, 150);
	else
		vga_fill_rect(cx, w->y + 1, w->pw - 2, WM_TITLE_H, 100, 100, 100);

	/* Title text. */
	fbtext_draw_string_xy(w->x + 4, w->y + 3, w->title, 255, 255, 255, 1);

	/* Close button. */
	{
		unsigned int bx = w->x + w->pw - 1 - WM_CLOSE_W;
		vga_fill_rect(bx, w->y + 1, WM_CLOSE_W, WM_TITLE_H, 190, 60, 60);
		fbtext_draw_string_xy(bx + 3, w->y + 3, "x", 255, 255, 255, 1);
	}

	/* Content (black background) + cell buffer. */
	vga_fill_rect(cx, cy, w->pw - 2, SCREEN_ROWS * 10, 0, 0, 0);
	fbtext_draw_cells(s->cells, s->cols, s->rows, cx, cy);

	/* Blinking block cursor for the focused window. */
	if (w->focused && s->cursor_visible) {
		unsigned int col = s->cursor % s->cols;
		unsigned int row = s->cursor / s->cols;
		vga_fill_rect(cx + col * 8, cy + row * 10, 2, 10, 255, 255, 255);
	}
}

static void wm_render_full(void)
{
	unsigned int fbw = vga_fb_width();
	unsigned int fbh = vga_fb_height();
	unsigned int i;

	/* Erase the mouse cursor so the scene is drawn onto a clean background. */
	mouse_cursor_erase();

	/* Desktop background. */
	vga_fill_rect(0, 0, fbw, fbh, 32, 32, 48);

	/* Windows back-to-front (focused drawn last = on top). */
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && !windows[i].focused)
			wm_draw_window(&windows[i]);
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].focused)
			wm_draw_window(&windows[i]);

	/* System bar always on top. */
	wm_draw_bar();

	/* Mouse cursor on top of everything. */
	mouse_redraw_cursor();
}

static void wm_render_bar(void)
{
	unsigned int fbw = vga_fb_width();
	mouse_cursor_erase();
	vga_fill_rect(0, 0, fbw, WM_BAR_H, 32, 32, 48);
	wm_draw_bar();
	mouse_redraw_cursor();
}

static void wm_redraw_if_needed(void)
{
	unsigned int i;
	static unsigned int last_sec = 0xFFFFFFFF;
	static unsigned int last_kexec_tick;
	unsigned int now_ms = get_ticks_ms();
	unsigned int now_sec = now_ms / 1000;
	int clock_changed = (now_sec != last_sec);
	int kexec_changed = kexec_busy && (now_ms - last_kexec_tick >= 100);
	int need_full = wm_full_redraw;

	if (kexec_changed)
		last_kexec_tick = now_ms;

	/* A background window's content changed -> full redraw (rare). */
	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && !windows[i].focused && windows[i].term->screen.dirty)
			need_full = 1;

	if (need_full) {
		wm_render_full();
		for (i = 0; i < MAX_WINDOWS; i++)
			if (windows[i].used)
				windows[i].term->screen.dirty = 0;
		wm_full_redraw = 0;
		last_sec = now_sec;
		return;
	}

	if (clock_changed || kexec_changed) {
		wm_render_bar();
		last_sec = now_sec;
	}

	/* Focused window's content changed -> redraw only that window. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (windows[i].used && windows[i].focused && windows[i].term->screen.dirty) {
			mouse_cursor_erase();
			wm_draw_window(&windows[i]);
			mouse_redraw_cursor();
			windows[i].term->screen.dirty = 0;
		}
	}
}

static void wm_init(void)
{
	unsigned int i;
	for (i = 0; i < MAX_WINDOWS; i++) {
		windows[i].used = 0;
		windows[i].term = &terms[i];
	}
	term_counter = 0;
	wm_full_redraw = 1;
	drag_active = 0;
	prev_buttons = 0;
	wm_spawn_terminal();
}

static void wm_blink_cursor(void)
{
	static unsigned int last_cursor_tick;
	struct window *fw = wm_focused_window();
	unsigned int now = get_ticks_ms();

	if (!fw || !fw->term)
		return;
	if (now - last_cursor_tick >= CURSOR_MS) {
		last_cursor_tick = now;
		fw->term->screen.cursor_visible = !fw->term->screen.cursor_visible;
		fw->term->screen.dirty = 1;
	}
}

static void wm_handle_kexec(void)
{
	if (!kexec_busy && kexec_pending()) {
		kexec_busy = 1;
		if (process_create("kexec", program_kexec_main) < 0)
			kexec_busy = 0;
	}
}

void wm_run(void)
{
	wm_init();
	for (;;) {
		mouse_poll_update();
		wm_handle_mouse();
		wm_handle_keyboard();
		wm_blink_cursor();
		wm_handle_kexec();
		wm_redraw_if_needed();
		vga_fb_flush_pending_check();
		cpu_idle();
	}
}
