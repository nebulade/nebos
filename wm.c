/* Window manager: fixed-size draggable terminal windows + a system bar.
   Slot 0 runs wm_run(); each terminal window is a separate process. */

#include "wm.h"
#include "cirrus.h"
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

/* Drag damage: accumulated y-band of the moving window, flushed as a region. */
static int drag_render_needed;
static int drag_band_valid;
static unsigned int drag_band_y0, drag_band_y1;
static unsigned int last_drag_render;

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
	t->cwd[0] = '/';
	t->cwd[1] = '\0';

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

/* Grow the drag's accumulated dirty band to cover the window's old+new rects.
   Only the y-range matters: the flush copies whole rows. */
static void wm_drag_accumulate(unsigned int oy, unsigned int ny, unsigned int ph)
{
	unsigned int y0 = (oy < ny) ? oy : ny;
	unsigned int y1 = ((oy > ny) ? oy : ny) + ph;
	if (!drag_band_valid) {
		drag_band_y0 = y0;
		drag_band_y1 = y1;
		drag_band_valid = 1;
	} else {
		if (y0 < drag_band_y0)
			drag_band_y0 = y0;
		if (y1 > drag_band_y1)
			drag_band_y1 = y1;
	}
}

/* Byte copy that handles overlapping source/destination. */
static void wm_memmove(unsigned char *dst, const unsigned char *src, unsigned int n)
{
	if (dst == src || n == 0)
		return;
	if (dst < src) {
		unsigned int i;
		for (i = 0; i < n; i++)
			dst[i] = src[i];
	} else {
		while (n--)
			dst[n] = src[n];
	}
}

/* Copy a rectangle within the shadow buffer (to mirror a BitBLT'd drag). */
static void wm_shadow_copy(unsigned int ox, unsigned int oy,
			   unsigned int nx, unsigned int ny,
			   unsigned int w, unsigned int h)
{
	unsigned char *fb = (unsigned char *)vga_fb_ptr();
	unsigned int pitch = vga_fb_pitch();
	unsigned int bpp = vga_fb_bpp();
	unsigned int pb = (bpp == 16) ? 2 : (bpp == 24) ? 3 : 4;
	unsigned int row_bytes = w * pb;
	unsigned int r;

	if (ny > oy || (ny == oy && nx > ox)) {
		for (r = h; r-- > 0; )
			wm_memmove(fb + (ny + r) * pitch + nx * pb,
				   fb + (oy + r) * pitch + ox * pb, row_bytes);
	} else {
		for (r = 0; r < h; r++)
			wm_memmove(fb + (ny + r) * pitch + nx * pb,
				   fb + (oy + r) * pitch + ox * pb, row_bytes);
	}
}

/* Fill the intersection of rect (rx,ry,rw,rh) with clip rect (cx,cy,cw,ch). */
static void wm_fill_rect_clip(unsigned int rx, unsigned int ry, unsigned int rw, unsigned int rh,
			      unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch,
			      unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int x0 = (rx > cx) ? rx : cx;
	unsigned int y0 = (ry > cy) ? ry : cy;
	unsigned int x1 = (rx + rw < cx + cw) ? rx + rw : cx + cw;
	unsigned int y1 = (ry + rh < cy + ch) ? ry + rh : cy + ch;
	if (x1 > x0 && y1 > y0)
		vga_fill_rect(x0, y0, x1 - x0, y1 - y0, r, g, b);
}

/* Redraw a rectangular region of the desktop (background + underlying windows,
   excluding the window being dragged). Restores the exposed sliver. */
static void wm_redraw_sliver(unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
	unsigned int i;
	vga_fill_rect(x, y, w, h, 32, 32, 48);
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct window *w2 = &windows[i];
		if (!w2->used || (int)i == drag_window)
			continue;
		if (!(x < w2->x + w2->pw && x + w > w2->x &&
		      y < w2->y + w2->ph && y + h > w2->y))
			continue;

		/* Border. */
		wm_fill_rect_clip(w2->x, w2->y, w2->pw, w2->ph,
			x, y, w, h, 200, 200, 200);
		/* Title bar. */
		if (w2->focused)
			wm_fill_rect_clip(w2->x + 1, w2->y + 1, w2->pw - 2, WM_TITLE_H,
				x, y, w, h, 70, 90, 150);
		else
			wm_fill_rect_clip(w2->x + 1, w2->y + 1, w2->pw - 2, WM_TITLE_H,
				x, y, w, h, 100, 100, 100);
		/* Content background + cells. */
		wm_fill_rect_clip(w2->x + 1, w2->y + 1 + WM_TITLE_H,
			w2->pw - 2, SCREEN_ROWS * 10, x, y, w, h, 0, 0, 0);
		fbtext_draw_cells_clip(w2->term->screen.cells,
			w2->term->screen.cols, w2->term->screen.rows,
			w2->x + 1, w2->y + 1 + WM_TITLE_H, x, y, w, h);
	}
}

/* Cirrus fast drag: move the window with the BitBLT, restore the exposed
   sliver, and mirror both in the RAM shadow buffer. No full recomposite. */
static void wm_drag_fast(struct window *w, unsigned int ox, unsigned int oy,
			 unsigned int nx, unsigned int ny)
{
	int dx = (int)nx - (int)ox;
	int dy = (int)ny - (int)oy;

	cirrus_blit(ox, oy, nx, ny, w->pw, w->ph);
	wm_shadow_copy(ox, oy, nx, ny, w->pw, w->ph);

	if (dy > 0)
		wm_redraw_sliver(ox, oy, w->pw, (unsigned int)dy);
	else if (dy < 0)
		wm_redraw_sliver(ox, ny + w->ph, w->pw, (unsigned int)(-dy));
	if (dx > 0)
		wm_redraw_sliver(ox, oy, (unsigned int)dx, w->ph);
	else if (dx < 0)
		wm_redraw_sliver(nx + w->pw, oy, (unsigned int)(-dx), w->ph);

	vga_fb_flush();
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
					drag_band_valid = 0;
					drag_render_needed = 0;
				}
			}
		}
	} else if (left_down && left_was_down && drag_active) {
		struct window *w = &windows[drag_window];
		if (w->used) {
			unsigned int ox = w->x, oy = w->y;
			wm_clamp_window(w, (int)mx - drag_dx, (int)my - drag_dy);
			if (ox != w->x || oy != w->y) {
				if (cirrus_available())
					wm_drag_fast(w, ox, oy, w->x, w->y);
				else {
					wm_drag_accumulate(oy, w->y, w->ph);
					drag_render_needed = 1;
				}
			}
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
		drag_render_needed = 0;
		drag_band_valid = 0;
		last_sec = now_sec;
		return;
	}

	/* Drag: redraw the full scene into the shadow buffer, but flush only the
	   accumulated y-band (the window's swept region) to VRAM. Throttled to the
	   flush rate so rapid mouse moves coalesce. */
	if (drag_render_needed && (now_ms - last_drag_render >= 33)) {
		last_drag_render = now_ms;
		wm_render_full();
		vga_fb_clear_dirty();
		if (drag_band_valid)
			vga_fb_mark_dirty_rect(drag_band_y0, drag_band_y1 - drag_band_y0);
		if (clock_changed)
			vga_fb_mark_dirty_rect(0, WM_BAR_H);
		for (i = 0; i < MAX_WINDOWS; i++)
			if (windows[i].used)
				windows[i].term->screen.dirty = 0;
		drag_render_needed = 0;
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
