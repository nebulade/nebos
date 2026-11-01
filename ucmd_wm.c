/* Ring-3 window manager: maps the shadow buffer and each window's content,
 * draws the desktop/chrome and composites the terminal cell grids or gfx
 * canvases.  All glyphs and colours are userland; the kernel only routes
 * input, stores window state and copies damage to VRAM. */

#include "ucmd_lib.h"
#include "uwm_lib.h"
#include "version.h"

#define COL_BG        0x202030u
#define COL_BORDER    0xC8C8C8u
#define COL_TITLE_ON  0x465A96u
#define COL_TITLE_OFF 0x646464u
#define COL_CLOSE     0xBE3C3Cu
#define COL_WHITE     0xFFFFFFu
#define COL_BANNER    0x55FF55u

struct uwin {
	struct wm_window_info info;
	struct wm_map_info map;
	int mapped;
	unsigned int ack_seq;
};

static struct usurf screen;
static struct uwin wins[WM_MAX_WINDOWS];
static unsigned int fbw, fbh;
static int drag_id = -1;
static int drag_dx, drag_dy;
static unsigned int prev_buttons;
static int full_redraw = 1;
static unsigned int bar_kexec_busy, bar_kexec_progress, bar_kexec_total;

static unsigned int rgb16(unsigned int r, unsigned int g, unsigned int b)
{
	return (r << 16) | (g << 8) | b;
}

static void fill_clip(int x, int y, unsigned int w, unsigned int h,
		      unsigned int rgb, int cx, int cy,
		      unsigned int cw, unsigned int ch)
{
	int x0 = x, y0 = y, x1 = x + (int)w, y1 = y + (int)h;

	if (x0 < cx) x0 = cx;
	if (y0 < cy) y0 = cy;
	if (x1 > cx + (int)cw) x1 = cx + (int)cw;
	if (y1 > cy + (int)ch) y1 = cy + (int)ch;
	if (x1 > x0 && y1 > y0)
		usurf_fill(&screen, x0, y0, (unsigned int)(x1 - x0),
			   (unsigned int)(y1 - y0), rgb);
}

static void draw_bar(void)
{
	char dt[48];
	unsigned int n, i, x, y;

	usurf_fill(&screen, 0, 0, fbw, WM_BAR_H, COL_BG);
	usurf_text(&screen, 4, 5, OS_NAME " v" OS_VERSION, COL_BANNER,
		   0, 0, fbw, WM_BAR_H);
	usurf_fill(&screen, WM_BTN_X, WM_BTN_Y, WM_BTN_W, WM_BTN_H, 0x963CD2u);
	usurf_text(&screen, WM_BTN_X + 4, WM_BTN_Y + 3, "+ New", COL_WHITE,
		   0, 0, fbw, WM_BAR_H);

	/* Rainbow separator (same y as the kernel's old bar). */
	for (x = 0; x < fbw; x++) {
		unsigned int seg = (x * (6 * 256)) / (fbw ? fbw : 1);
		unsigned int s = seg / 256, f = seg % 256;
		unsigned int r1, g1, b1, r2, g2, b2, r, g, b;

		switch (s) {
		case 0: r1=255; g1=0;   b1=0;   r2=255; g2=255; b2=0;   break;
		case 1: r1=255; g1=255; b1=0;   r2=0;   g2=255; b2=0;   break;
		case 2: r1=0;   g1=255; b1=0;   r2=0;   g2=255; b2=255; break;
		case 3: r1=0;   g1=255; b1=255; r2=0;   g2=0;   b2=255; break;
		case 4: r1=0;   g1=0;   b1=255; r2=255; g2=0;   b2=255; break;
		default:r1=255; g1=0;   b1=255; r2=255; g2=0;   b2=0;   break;
		}
		r = (r1 * (256 - f) + r2 * f) / 256;
		g = (g1 * (256 - f) + g2 * f) / 256;
		b = (b1 * (256 - f) + b2 * f) / 256;
		for (y = 18; y < 20; y++)
			usurf_fill(&screen, (int)x, (int)y, 1, 1, rgb16(r, g, b));
	}

	if (bar_kexec_busy) {
		static const char msg[] = "loading new kernel over serial";
		unsigned int dots = bar_kexec_total ?
			(bar_kexec_progress * 12 / bar_kexec_total) : 0;

		n = 0;
		while (msg[n])
			dt[n] = msg[n], n++;
		for (i = 0; i < 12; i++)
			dt[n + i] = (i < dots) ? '.' : ' ';
		n += 12;
		dt[n] = '\0';
		usurf_text(&screen, (int)fbw - (int)n * 8 - 4, 5, dt,
			   0xFFC864u, 0, 0, fbw, WM_BAR_H);
	} else if (u_rtc_datetime(dt, sizeof(dt)) == 0) {
		n = 0;
		while (dt[n] && n < sizeof(dt) - 1)
			n++;
		usurf_text(&screen, (int)fbw - (int)n * 8 - 4, 5, dt,
			   COL_BANNER, 0, 0, fbw, WM_BAR_H);
	}
}

static void draw_window_clip(unsigned int idx, int cx, int cy,
			     unsigned int cw, unsigned int ch)
{
	struct uwin *u = &wins[idx];
	struct wm_window_info *w = &u->info;
	int x = w->x, y = w->y;
	unsigned int pw = w->w, ph = w->h;
	unsigned int bx = (unsigned int)(x + (int)pw - 1 - WM_CLOSE_W);
	unsigned int cony = (unsigned int)(y + 1 + WM_TITLE_H);
	unsigned int content_x = (unsigned int)(x + 1);
	unsigned int content_w = w->content_w;
	unsigned int content_h = w->content_h;

	/* Border, title bar, close button. */
	fill_clip(x, y, pw, ph, COL_BORDER, cx, cy, cw, ch);
	fill_clip(x + 1, y + 1, pw - 2, WM_TITLE_H,
		  w->focused ? COL_TITLE_ON : COL_TITLE_OFF, cx, cy, cw, ch);
	usurf_text(&screen, x + 4, y + 3, w->title, COL_WHITE,
		   cx, cy, cw, ch);
	fill_clip((int)bx, y + 1, WM_CLOSE_W, WM_TITLE_H, COL_CLOSE,
		  cx, cy, cw, ch);
	usurf_text(&screen, (int)bx + 3, y + 3, "x", COL_WHITE,
		   cx, cy, cw, ch);

	/* Content. */
	fill_clip((int)content_x, (int)cony, content_w, content_h,
		  0x000000u, cx, cy, cw, ch);
	if (u->mapped) {
		if (w->kind == WM_KIND_GFX) {
			struct uwm_canvas c;

			c.px = (const unsigned short *)u->map.va;
			c.w = u->map.w;
			c.h = u->map.h;
			usurf_canvas(&screen, &c, (int)content_x, (int)cony,
				     cx, cy, cw, ch);
		} else if (u->map.kind == WM_KIND_TEXT) {
			usurf_cells(&screen, (const unsigned short *)u->map.va,
				    w->cols, w->rows, (int)content_x, (int)cony,
				    cx, cy, cw, ch);
		}
	}

	/* Text cursor overlay (only for the focused terminal). */
	if (w->kind == WM_KIND_TEXT && w->focused && w->cursor_visible &&
	    w->cols && w->rows && w->cursor < w->cols * w->rows) {
		unsigned int col = w->cursor % w->cols;
		unsigned int row = w->cursor / w->cols;
		unsigned int px = content_x + col * 8;
		unsigned int py = cony + row * 10;

		fill_clip((int)px, (int)py, 2, 10, COL_WHITE,
			  cx, cy, cw, ch);
	}
}

static void draw_rect(int x, int y, unsigned int w, unsigned int h)
{
	unsigned int i;

	if (w == 0 || h == 0 || x >= (int)fbw || y >= (int)fbh)
		return;
	usurf_fill(&screen, x, y, w, h, COL_BG);
	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (wins[i].info.used && !wins[i].info.focused &&
		    x < wins[i].info.x + (int)wins[i].info.w &&
		    x + (int)w > wins[i].info.x &&
		    y < wins[i].info.y + (int)wins[i].info.h &&
		    y + (int)h > wins[i].info.y)
			draw_window_clip(i, x, y, w, h);
	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (wins[i].info.used && wins[i].info.focused &&
		    x < wins[i].info.x + (int)wins[i].info.w &&
		    x + (int)w > wins[i].info.x &&
		    y < wins[i].info.y + (int)wins[i].info.h &&
		    y + (int)h > wins[i].info.y)
			draw_window_clip(i, x, y, w, h);
}

static int window_at(unsigned int mx, unsigned int my)
{
	unsigned int i;

	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (wins[i].info.used && wins[i].info.focused &&
		    mx >= (unsigned int)wins[i].info.x &&
		    my >= (unsigned int)wins[i].info.y &&
		    mx < (unsigned int)(wins[i].info.x + (int)wins[i].info.w) &&
		    my < (unsigned int)(wins[i].info.y + (int)wins[i].info.h))
			return (int)i;
	for (i = 0; i < WM_MAX_WINDOWS; i++)
		if (wins[i].info.used && !wins[i].info.focused &&
		    mx >= (unsigned int)wins[i].info.x &&
		    my >= (unsigned int)wins[i].info.y &&
		    mx < (unsigned int)(wins[i].info.x + (int)wins[i].info.w) &&
		    my < (unsigned int)(wins[i].info.y + (int)wins[i].info.h))
			return (int)i;
	return -1;
}

static void clamp_geom(struct uwin *u, int *x, int *y)
{
	int maxx = (int)fbw - (int)u->info.w;
	int maxy = (int)fbh - (int)u->info.h;

	if (*x < 0) *x = 0;
	if (*y < WM_BAR_H) *y = WM_BAR_H;
	if (maxx < 0) maxx = 0;
	if (maxy < WM_BAR_H) maxy = WM_BAR_H;
	if (*x > maxx) *x = maxx;
	if (*y > maxy) *y = maxy;
}

static void add_rect(struct wm_present_info *p, int x, int y,
		     unsigned int w, unsigned int h)
{
	if (w == 0 || h == 0)
		return;
	if (p->count < 4) {
		p->rect[p->count].x = x;
		p->rect[p->count].y = y;
		p->rect[p->count].w = w;
		p->rect[p->count].h = h;
		p->count++;
	} else {
		/* Four is enough for normal updates; fold the rest into the
		 * last rect rather than dropping pixels. */
		int x0 = p->rect[3].x, y0 = p->rect[3].y;
		int x1 = x0 + (int)p->rect[3].w, y1 = y0 + (int)p->rect[3].h;
		int nx1 = x + (int)w, ny1 = y + (int)h;

		if (x < x0) x0 = x;
		if (y < y0) y0 = y;
		if (nx1 > x1) x1 = nx1;
		if (ny1 > y1) y1 = ny1;
		p->rect[3].x = x0;
		p->rect[3].y = y0;
		p->rect[3].w = (unsigned int)(x1 - x0);
		p->rect[3].h = (unsigned int)(y1 - y0);
	}
}

static void do_update(struct wm_status *st)
{
	struct wm_present_info present;
	int need = 0;
	int bar_dirty = 0;
	unsigned int i;
	char dt[20];
	static char last_dt[20];
	int kexec_changed = (st->kexec_busy != bar_kexec_busy) ||
		(st->kexec_busy && st->kexec_progress != bar_kexec_progress);

	bar_kexec_busy = st->kexec_busy;
	bar_kexec_progress = st->kexec_progress;
	bar_kexec_total = st->kexec_total;
	if (kexec_changed)
		bar_dirty = 1;

	/* Refresh cached window state and mappings before opening a frame:
	 * MAP may unmap/remap content. */
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		struct wm_window_info info;
		int r = u_wm_info(i, &info);

		if (r < 0) {
			u_exit(1);
		}
		if (r == 0 || !info.used) {
			if (wins[i].info.used)
				full_redraw = 1;
			wins[i].info.used = 0;
			wins[i].mapped = 0;
			continue;
		}
		if (!wins[i].info.used || info.content_gen != wins[i].info.content_gen ||
		    info.kind != wins[i].info.kind)
			full_redraw = 1;
		wins[i].info = info;
		if (!wins[i].mapped || wins[i].map.kind != info.kind ||
		    wins[i].map.generation != info.content_gen) {
			struct wm_map_info map;

			if (u_wm_map(i, &map) == 0) {
				wins[i].map = map;
				wins[i].mapped = 1;
			} else {
				wins[i].mapped = 0;
			}
		}
	}
	if (u_rtc_datetime(dt, sizeof(dt)) == 0) {
		for (i = 0; i < sizeof(dt); i++)
			if (dt[i] != last_dt[i]) {
				for (i = 0; i < sizeof(dt); i++)
					last_dt[i] = dt[i];
				bar_dirty = 1;
				break;
			}
	}
	if (full_redraw) {
		if (u_wm_begin() != 0) {
			full_redraw = 1;
			return;
		}
		full_redraw = 0;
		need = 1;
		{
			usurf_fill(&screen, 0, 0, fbw, fbh, COL_BG);
			for (i = 0; i < WM_MAX_WINDOWS; i++)
				if (wins[i].info.used && !wins[i].info.focused)
					draw_window_clip(i, 0, 0, fbw, fbh);
			for (i = 0; i < WM_MAX_WINDOWS; i++)
				if (wins[i].info.used && wins[i].info.focused)
					draw_window_clip(i, 0, 0, fbw, fbh);
			draw_bar();
			present.count = 0;
			add_rect(&present, 0, 0, fbw, fbh);
			present.immediate = 0;
			u_wm_present(&present);
			/* Ack every window that was clean at the time of the
			 * redraw.  If new damage arrived during it, its seq
			 * differs and the kernel leaves it dirty. */
			for (i = 0; i < WM_MAX_WINDOWS; i++)
				if (wins[i].info.used && wins[i].mapped)
					u_wm_ack(i, wins[i].info.dirty_seq);
		}
		return;
	}

	/* Partial cell/canvas updates and the bar clock. */
	present.count = 0;
	present.immediate = 0;
	if (bar_dirty) {
		need = 1;
		add_rect(&present, 0, 0, fbw, WM_BAR_H);
	}
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		struct uwin *u = &wins[i];
		struct wm_window_info *w = &u->info;
		int rx, ry;
		unsigned int rw, rh;

		if (!w->used || !w->dirty)
			continue;
		if (w->kind == WM_KIND_GFX) {
			rx = w->x + 1;
			ry = w->y + 1 + WM_TITLE_H;
			rw = w->content_w;
			rh = w->content_h;
		} else {
			if (!w->dmg_cols || !w->dmg_rows)
				continue;
			rx = w->x + 1 + (int)w->dmg_col0 * 8;
			ry = w->y + 1 + WM_TITLE_H + (int)w->dmg_row0 * 10;
			rw = w->dmg_cols * 8;
			rh = w->dmg_rows * 10;
		}
		add_rect(&present, rx, ry, rw, rh);
		u->ack_seq = u->mapped ? w->dirty_seq : 0;
		need = 1;
	}

	if (!need) {
		/* Drag movement is handled below; a stationary mouse can be
		 * left to the kernel's cursor overlay. */
		return;
	}
	if (u_wm_begin() != 0)
		return;

	for (i = 0; i < present.count; i++)
		draw_rect(present.rect[i].x, present.rect[i].y,
			  present.rect[i].w, present.rect[i].h);
	if (bar_dirty)
		draw_bar();

	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		struct uwin *u = &wins[i];

		if (!u->info.used || !u->info.dirty || !u->mapped ||
		    !u->ack_seq)
			continue;
		if (u->ack_seq == u->info.dirty_seq)
			u_wm_ack(i, u->info.dirty_seq);
	}
	u_wm_present(&present);
}

static void handle_mouse(struct wm_status *st)
{
	unsigned int buttons = st->mouse.buttons;
	unsigned int mx = st->mouse.x, my = st->mouse.y;
	int down = (buttons & 1) != 0;
	int was = (prev_buttons & 1) != 0;

	if (down && !was) {
		if (mx >= WM_BTN_X && my >= WM_BTN_Y &&
		    mx < WM_BTN_X + WM_BTN_W && my < WM_BTN_Y + WM_BTN_H) {
			u_wm_create_term();
			full_redraw = 1;
		} else {
			int idx = window_at(mx, my);

			if (idx >= 0) {
				struct uwin *u = &wins[idx];

				u_wm_focus((unsigned int)idx);
				if (mx >= (unsigned int)(u->info.x + (int)u->info.w -
							 1 - WM_CLOSE_W) &&
				    mx < (unsigned int)(u->info.x + (int)u->info.w) &&
				    my >= (unsigned int)(u->info.y + 1) &&
				    my < (unsigned int)(u->info.y + 1 + WM_TITLE_H)) {
					u_wm_close((unsigned int)idx);
					full_redraw = 1;
				} else if (my < (unsigned int)(u->info.y + WM_TITLE_H)) {
					drag_id = idx;
					drag_dx = (int)mx - u->info.x;
					drag_dy = (int)my - u->info.y;
				}
			}
		}
	} else if (down && was && drag_id >= 0) {
		struct uwin *u = &wins[drag_id];

		if (u->info.used) {
			int ox = u->info.x, oy = u->info.y;
			int nx = (int)mx - drag_dx;
			int ny = (int)my - drag_dy;

			clamp_geom(u, &nx, &ny);
			if (nx != ox || ny != oy) {
				struct wm_present_info present;
				unsigned int i;

				u_wm_set_geom((unsigned int)drag_id, nx, ny);
				u->info.x = nx;
				u->info.y = ny;
				present.count = 0;
				present.immediate = 1;
				add_rect(&present, ox, oy, u->info.w, u->info.h);
				add_rect(&present, nx, ny, u->info.w, u->info.h);
				if (u_wm_begin() == 0) {
					for (i = 0; i < present.count; i++)
						draw_rect(present.rect[i].x,
							  present.rect[i].y,
							  present.rect[i].w,
							  present.rect[i].h);
					u_wm_present(&present);
				}
			}
		} else {
			drag_id = -1;
		}
	} else if (!down && was) {
		drag_id = -1;
	}
	prev_buttons = buttons;
}

void _start(const char *arg)
{
	struct wm_screen_info si;
	struct wm_status st;
	unsigned int i;

	(void)arg;
	if (u_wm_attach(&si) != 0)
		u_exit(1);
	usurf_init(&screen, si.fb, si.width, si.height, si.pitch, si.bpp);
	fbw = si.width;
	fbh = si.height;
	for (i = 0; i < WM_MAX_WINDOWS; i++) {
		wins[i].info.used = 0;
		wins[i].mapped = 0;
	}
	full_redraw = 1;

	for (;;) {
		if (u_wm_poll(&st) != 0)
			u_exit(1);
		if (st.full_redraw)
			full_redraw = 1;
		handle_mouse(&st);
		do_update(&st);
		u_sleep_ms(10);
	}
}
