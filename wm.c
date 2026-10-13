/* Window manager: fixed-size draggable terminal windows + a system bar.
   Slot 0 runs wm_run(); each terminal window is a separate process. */

#include "wm.h"
#include "cirrus.h"
#include "fbtext.h"
#include "idle.h"
#include "io.h"
#include "kdbg.h"
#include "kexec.h"
#include "mouse.h"
#include "netkexec.h"
#include "netlog.h"
#include "payload.h"
#include "process.h"
#include "rtc.h"
#include "sys_gfx.h"
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

/* The terminal whose screen is `screen`, or NULL: a spawned ring-3 command
 * inherits its terminal's screen pointer, so this maps it back to the window
 * (used by the sys_gfx window-canvas service). */
struct term *wm_term_for_screen(struct text_screen *screen)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term &&
		    &windows[i].term->screen == screen)
			return windows[i].term;
	return 0;
}

/* Pixel size of a window's content area (the size of its gfx canvas). */
int wm_content_size(struct term *t, unsigned int *w, unsigned int *h)
{
	unsigned int i;

	for (i = 0; i < MAX_WINDOWS; i++)
		if (windows[i].used && windows[i].term == t) {
			*w = windows[i].pw - 2;
			*h = SCREEN_ROWS * 10;
			return 0;
		}
	return -1;
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
	sys_gfx_kill_for_screen(&w->term->screen);   /* stop a running demo */
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

/* Overlap-safe copy.  Moving a window copies most of its area, so take four
   bytes at a time when both rows are dword aligned (32 bpp rows always are)
   and fall back to bytes otherwise (24 bpp at an odd x). */
static void wm_memmove(unsigned char *dst, const unsigned char *src, unsigned int n)
{
	if (dst == src || n == 0)
		return;

	if ((((unsigned int)dst | (unsigned int)src) & 3) == 0) {
		unsigned int words = n >> 2;
		unsigned int i;
		unsigned int *dw = (unsigned int *)dst;
		const unsigned int *sw = (const unsigned int *)src;

		if (dst < src) {
			for (i = 0; i < words; i++)
				dw[i] = sw[i];
			for (i = words << 2; i < n; i++)
				dst[i] = src[i];
		} else {
			/* Overlapping move down: back to front. */
			for (i = n; i > (words << 2); i--)
				dst[i - 1] = src[i - 1];
			for (i = words; i-- > 0; )
				dw[i] = sw[i];
		}
		return;
	}

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

static void wm_draw_window_clip(struct window *w, unsigned int cx, unsigned int cy,
				unsigned int cw, unsigned int ch);

/* Repaint the desktop rectangle (x, y, w, h): background, then every
   intersecting window back-to-front (focused last), then mark it dirty.  The
   caller handles the mouse cursor.  The rect must lie below the system bar
   (windows are clamped to y >= WM_BAR_H, so the bar is never overlapped and
   needs no clip variant). */
static void wm_redraw_rect(unsigned int x, unsigned int y,
			   unsigned int w, unsigned int h)
{
	unsigned int i;

	if (w == 0 || h == 0)
		return;
	vga_fill_rect(x, y, w, h, 32, 32, 48);
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct window *w2 = &windows[i];

		if (w2->used && !w2->focused &&
		    x < w2->x + w2->pw && x + w > w2->x &&
		    y < w2->y + w2->ph && y + h > w2->y)
			wm_draw_window_clip(w2, x, y, w, h);
	}
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct window *w2 = &windows[i];

		if (w2->used && w2->focused &&
		    x < w2->x + w2->pw && x + w > w2->x &&
		    y < w2->y + w2->ph && y + h > w2->y)
			wm_draw_window_clip(w2, x, y, w, h);
	}
	vga_fb_mark_dirty_rect_xy(y, h, x, x + w);
}

/* Move a window in the shadow buffer: copy its rect, redraw the exposed
   sliver, keep the cursor on top, and flush the swept band immediately so a
   drag step is always one consistent frame (a budgeted flush would copy only
   the top rows of the band per iteration and the window would appear to
   stretch while the mouse keeps moving). */
static void wm_drag_shadow(struct window *w, unsigned int ox, unsigned int oy,
			   unsigned int nx, unsigned int ny)
{
	int dx = (int)nx - (int)ox;
	int dy = (int)ny - (int)oy;

	mouse_cursor_erase();
	wm_shadow_copy(ox, oy, nx, ny, w->pw, w->ph);

	if (dy > 0)
		wm_redraw_rect(ox, oy, w->pw, (unsigned int)dy);
	else if (dy < 0)
		wm_redraw_rect(ox, ny + w->ph, w->pw, (unsigned int)(-dy));
	if (dx > 0)
		wm_redraw_rect(ox, oy, (unsigned int)dx, w->ph);
	else if (dx < 0)
		wm_redraw_rect(nx + w->pw, oy, (unsigned int)(-dx), w->ph);

	/* Only the moved window's new rect needs marking: the repaints above
	   cover the exposed slivers exactly, and the cursor marks itself.  The
	   damage list merges these when the step is small (the union is then
	   the swept hull anyway) and keeps them apart after a big jump. */
	vga_fb_mark_dirty_rect_xy(w->y, w->ph, w->x, w->x + w->pw);
	mouse_redraw_cursor();
	vga_fb_flush();
}

/* Cirrus fast drag: also move the window in VRAM with the BitBLT engine
   (wm_drag_shadow flushes the mirrored copy in the shadow buffer). */
static void wm_drag_fast(struct window *w, unsigned int ox, unsigned int oy,
			 unsigned int nx, unsigned int ny)
{
	cirrus_blit(ox, oy, nx, ny, w->pw, w->ph);
	wm_drag_shadow(w, ox, oy, nx, ny);
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
			unsigned int ox = w->x, oy = w->y;
			wm_clamp_window(w, (int)mx - drag_dx, (int)my - drag_dy);
			if (ox != w->x || oy != w->y) {
				if (cirrus_available())
					wm_drag_fast(w, ox, oy, w->x, w->y);
				else
					wm_drag_shadow(w, ox, oy, w->x, w->y);
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
	static unsigned int last_hw_poll;
	unsigned int now = get_ticks_ms();
	int handled = 0;
	int n;

	if (io_keyboard_raw())
		return;   /* an inline command is polling for keys itself */

	if (!fw || !fw->term)
		return;

	for (n = 0; n < 8; n++) {
		int k = io_poll_key();
		if (!k && (now - last_hw_poll) >= (kdbg_irq_masked ? 5 : 50)) {
			/* Fallback paths: IRQ1 was storm-masked, or no IRQ is
			 * delivering; sample the controller directly (faster
			 * when masked, so fast typing isn't dropped). */
			k = io_poll_key_with_hw();
			last_hw_poll = now;
		}
		if (!k)
			break;
		kdbg_wm_count++;
		kdbg_wm_last = (unsigned int)k;
		handled = 1;
		/* A graphics command owns its window's keys (ESC etc.);
		 * Ctrl+C always reaches the terminal so its foreground kill
		 * keeps working. */
		if (k == IO_KEY_CTRL_C ||
		    !sys_gfx_push_key(&fw->term->screen, k))
			term_key_push(fw->term, k);
	}
	if (handled)
		kdbg_status();
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

/* Draw window w clipped to (cx, cy, cw, ch).  Chrome uses wm_fill_rect_clip,
 * content the cells/canvas clip variants, so pixels outside the clip are
 * untouched. */
static void wm_draw_window_clip(struct window *w, unsigned int cx, unsigned int cy,
				unsigned int cw, unsigned int ch)
{
	struct text_screen *s = &w->term->screen;
	struct gfx_canvas *g = sys_gfx_canvas_for(&w->term->screen);
	unsigned int bx = w->x + w->pw - 1 - WM_CLOSE_W;
	unsigned int cony = w->y + 1 + WM_TITLE_H;

	/* Border. */
	wm_fill_rect_clip(w->x, w->y, w->pw, w->ph, cx, cy, cw, ch, 200, 200, 200);

	/* Title bar. */
	if (w->focused)
		wm_fill_rect_clip(w->x + 1, w->y + 1, w->pw - 2, WM_TITLE_H,
			cx, cy, cw, ch, 70, 90, 150);
	else
		wm_fill_rect_clip(w->x + 1, w->y + 1, w->pw - 2, WM_TITLE_H,
			cx, cy, cw, ch, 100, 100, 100);

	/* Title text. */
	fbtext_draw_string_xy_clip(w->x + 4, w->y + 3, w->title, 255, 255, 255,
		cx, cy, cw, ch);

	/* Close button. */
	wm_fill_rect_clip(bx, w->y + 1, WM_CLOSE_W, WM_TITLE_H,
		cx, cy, cw, ch, 190, 60, 60);
	fbtext_draw_string_xy_clip(bx + 3, w->y + 3, "x", 255, 255, 255,
		cx, cy, cw, ch);

	/* Content: text cells or the gfx canvas. */
	wm_fill_rect_clip(w->x + 1, cony, w->pw - 2, SCREEN_ROWS * 10,
		cx, cy, cw, ch, 0, 0, 0);
	if (g)
		gfx_blit_clip(g, w->x + 1, cony, cx, cy, cw, ch);
	else
		fbtext_draw_cells_clip(s->cells, s->cols, s->rows,
			w->x + 1, cony, cx, cy, cw, ch);

	/* Blinking block cursor (text windows only). */
	if (!g && w->focused && s->cursor_visible) {
		unsigned int col = s->cursor % s->cols;
		unsigned int row = s->cursor / s->cols;

		wm_fill_rect_clip(w->x + 1 + col * 8, cony + row * 10, 2, 10,
			cx, cy, cw, ch, 255, 255, 255);
	}
}

static void wm_draw_window(struct window *w)
{
	wm_draw_window_clip(w, w->x, w->y, w->pw, w->ph);
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
	int kexec_changed = kexec_busy && (now_ms - last_kexec_tick >= 100);
	int bar_changed;

	if (kexec_changed)
		last_kexec_tick = now_ms;
	bar_changed = (now_sec != last_sec) || kexec_changed;

	if (wm_full_redraw) {
		unsigned int seq_before[MAX_WINDOWS];

		for (i = 0; i < MAX_WINDOWS; i++) {
			struct gfx_canvas *g = windows[i].used ?
				sys_gfx_canvas_for(&windows[i].term->screen) : 0;

			seq_before[i] = g ? g->seq : 0;
		}
		wm_render_full();
		for (i = 0; i < MAX_WINDOWS; i++)
			if (windows[i].used) {
				struct gfx_canvas *g = sys_gfx_canvas_for(&windows[i].term->screen);

				screen_clear_damage(&windows[i].term->screen);
				if (g)
					g->drawn = seq_before[i];   /* keep a frame
						presented during the render */
			}
		wm_full_redraw = 0;
		last_sec = now_sec;
		return;
	}

	if (bar_changed) {
		wm_render_bar();
		last_sec = now_sec;
	}

	/* Any window whose content changed (focused or not): repaint just its
	   damaged cell block (a canvas window repaints its whole content)
	   through the damage-rect compositor. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct text_screen *s = &windows[i].term->screen;
		struct gfx_canvas *g;
		unsigned int seq, col0, row0, ncols, nrows;

		if (!windows[i].used)
			continue;
		g = sys_gfx_canvas_for(s);
		if (!screen_damage_block(s, &col0, &row0, &ncols, &nrows) &&
		    !(g && g->seq != g->drawn))
			continue;
		seq = g ? g->seq : 0;   /* capture before compositing */
		mouse_cursor_erase();
		if (g)
			wm_redraw_rect(windows[i].x + 1,
				windows[i].y + 1 + WM_TITLE_H,
				windows[i].pw - 2, SCREEN_ROWS * 10);
		else
			wm_redraw_rect(windows[i].x + 1 + col0 * 8,
				windows[i].y + 1 + WM_TITLE_H + row0 * 10,
				ncols * 8, nrows * 10);
		mouse_redraw_cursor();
		screen_clear_damage(s);
		if (g)
			g->drawn = seq;
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
	if (sys_gfx_canvas_for(&fw->term->screen))
		return;   /* no text cursor over a graphics command */
	if (now - last_cursor_tick >= CURSOR_MS) {
		last_cursor_tick = now;
		fw->term->screen.cursor_visible = !fw->term->screen.cursor_visible;
		screen_damage_cursor(&fw->term->screen);
	}
}

static void wm_handle_kexec(void)
{
	if (kexec_busy)
		return;
	/* A command pack for /ramfs (checked before kexec: both start with
	 * 'K', and the kexec detector drops anything that is not KXEC). */
	if (payload_pending()) {
		kexec_busy = 1;
		if (process_create("payload", program_payload_main) < 0)
			kexec_busy = 0;
		return;
	}
	if (kexec_pending()) {
		kexec_busy = 1;
		if (process_create("kexec", program_kexec_main) < 0)
			kexec_busy = 0;
		return;
	}
	/* Same over UDP: the host's `make net-dev` is picked up from here. */
	if (netkexec_pending()) {
		kexec_busy = 1;
		if (process_create("netkexec", program_netkexec_main) < 0)
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
		netlog_flush();
		wm_handle_kexec();
		sys_gfx_collect();
		wm_redraw_if_needed();
		vga_fb_flush_budget(VGA_FLUSH_BUDGET_BYTES);
		cpu_idle();
	}
}
