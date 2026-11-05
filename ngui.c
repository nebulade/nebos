/* ngui: immediate-mode widgets for RGB565 canvas windows.  See ngui.h. */

#include "ngui.h"
#include "ucmd_lib.h"
#include "font_8x16.h"

const struct ngui_theme ngui_theme_dark = {
	.bg          = NGUI_RGB(24, 28, 40),
	.panel       = NGUI_RGB(38, 43, 58),
	.panel_alt   = NGUI_RGB(50, 56, 74),
	.border      = NGUI_RGB(92, 99, 124),
	.text        = NGUI_RGB(228, 230, 240),
	.text_dim    = NGUI_RGB(145, 151, 170),
	.accent      = NGUI_RGB(84, 132, 224),
	.selection   = NGUI_RGB(54, 92, 166),
	.button      = NGUI_RGB(54, 61, 80),
	.button_hot  = NGUI_RGB(70, 81, 108),
	.button_down = NGUI_RGB(48, 112, 204),
	.icon_folder = NGUI_RGB(238, 198, 88),
	.icon_file   = NGUI_RGB(204, 210, 226),
	.icon_disk   = NGUI_RGB(112, 174, 232),
	.error       = NGUI_RGB(238, 116, 116),
};

static unsigned int slen(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void scopy(char *dst, const char *src, unsigned int cap)
{
	unsigned int i = 0;

	if (!cap)
		return;
	while (src && src[i] && i + 1 < cap) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int rect_empty(const struct ngui_rect *r)
{
	return r->w <= 0 || r->h <= 0;
}

static int rect_intersect(const struct ngui_rect *a, const struct ngui_rect *b,
			  struct ngui_rect *out)
{
	int x0 = a->x > b->x ? a->x : b->x;
	int y0 = a->y > b->y ? a->y : b->y;
	int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
	int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;

	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
	return !rect_empty(out);
}

static struct ngui_rect rect_make(int x, int y, int w, int h)
{
	struct ngui_rect r;

	r.x = x;
	r.y = y;
	r.w = w;
	r.h = h;
	return r;
}

int ngui_rect_contains(const struct ngui_rect *r, int x, int y)
{
	return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

void ngui_init(struct ngui *ui, unsigned short *fb, int w, int h)
{
	unsigned int i;

	for (i = 0; i < sizeof(*ui); i++)
		((unsigned char *)ui)[i] = 0;
	ui->px = fb;
	ui->w = w;
	ui->h = h;
	ui->theme = ngui_theme_dark;
	ui->nclip = 0;
	ui->dirty = 1;
	ui->caret_on = 1;
	ui->last_blink = u_ticks_ms();
	ui->motion_ms = ui->last_blink;
	ui->last_mouse_x = -1;
	ui->last_mouse_y = -1;
}

void ngui_invalidate(struct ngui *ui)
{
	ui->dirty = 1;
}

void ngui_set_focus(struct ngui *ui, unsigned int id)
{
	if (ui->focus != id) {
		ui->focus = id;
		ui->last_blink = u_ticks_ms();
		ui->caret_on = 1;
		ui->dirty = 1;
	}
}

unsigned int ngui_focus(struct ngui *ui)
{
	return ui->focus;
}

static void key_push(struct ngui *ui, int k)
{
	unsigned int next = (ui->key_head + 1) % 32;

	if (next == ui->key_tail)
		return;
	ui->keys[ui->key_head] = k;
	ui->key_head = next;
}

int ngui_key_peek(struct ngui *ui)
{
	if (ui->key_tail == ui->key_head)
		return 0;
	return ui->keys[ui->key_tail];
}

int ngui_key(struct ngui *ui)
{
	int k = ngui_key_peek(ui);

	if (!k)
		return 0;
	ui->key_tail = (ui->key_tail + 1) % 32;
	return k;
}

int ngui_begin(struct ngui *ui)
{
	struct win_info wi;
	struct win_pointer wp;
	struct win_event ev;
	unsigned int now;
	int want = ui->dirty;
	int i, k;

	ui->dirty = 0;
	ui->pressed = ui->released = 0;
	ui->clicked = 0;
	ui->focus_claimed = 0;
	ui->hot = 0;

	if (u_win_info(&wi) == 0) {
		if (wi.width != (unsigned int)ui->w ||
		    wi.height != (unsigned int)ui->h) {
			if (u_win_ack_resize() == 0) {
				ui->px = (unsigned short *)wi.fb;
				ui->w = (int)wi.width;
				ui->h = (int)wi.height;
			}
			want = 1;
		} else if (wi.fb && wi.fb != (void *)ui->px) {
			ui->px = (unsigned short *)wi.fb;
		}
	}

	if (u_win_pointer(&wp) == 0) {
		ui->mouse_x = wp.x;
		ui->mouse_y = wp.y;
		ui->buttons = wp.buttons;
		ui->inside = wp.inside ? 1 : 0;
		ui->focused = wp.focused ? 1 : 0;
	} else {
		ui->mouse_x = ui->mouse_y = 0;
		ui->buttons = 0;
		ui->inside = 0;
		ui->focused = 0;
	}

	for (i = 0; i < 16; i++) {
		int r = u_win_event(&ev);

		if (r != 1)
			break;
		if (ev.type == WIN_EV_POINTER_DOWN) {
			ui->pressed |= 1;
			ui->press_x = ev.x;
			ui->press_y = ev.y;
			ui->clicked = 1;
		} else if (ev.type == WIN_EV_POINTER_UP) {
			ui->released |= 1;
			ui->release_x = ev.x;
			ui->release_y = ev.y;
		}
	}

	for (i = 0; i < 32; i++) {
		k = u_key_poll();
		if (!k)
			break;
		key_push(ui, k);
		want = 1;
	}

	now = u_ticks_ms();
	if (ui->focus) {
		if (now - ui->last_blink >= 500) {
			ui->last_blink = now;
			ui->caret_on = !ui->caret_on;
			want = 1;
		}
	} else {
		ui->caret_on = 1;
	}

	if (ui->inside != ui->last_inside) {
		want = 1;
		ui->motion_pending = 0;
		ui->motion_ms = now;
	} else if (ui->inside &&
		   (ui->mouse_x != ui->last_mouse_x ||
		    ui->mouse_y != ui->last_mouse_y)) {
		/* Hover feedback needs a repaint, but not one per mouse
		 * report: while the pointer keeps moving, redraw at most every
		 * 33 ms and remember that a final frame is still due.  A held
		 * button is a drag, which stays at full rate. */
		if (ui->buttons) {
			ui->motion_ms = now;
			ui->motion_pending = 0;
			want = 1;
		} else {
			ui->motion_pending = 1;
			if ((unsigned int)(now - ui->motion_ms) >=
			    NGUI_MOTION_REDRAW_MS) {
				ui->motion_ms = now;
				ui->motion_pending = 0;
				want = 1;
			}
		}
	} else if (ui->motion_pending &&
		   (unsigned int)(now - ui->motion_ms) >= NGUI_MOTION_REDRAW_MS) {
		ui->motion_ms = now;
		ui->motion_pending = 0;
		want = 1;
	}
	if (ui->focused != ui->last_focused)
		want = 1;
	ui->last_mouse_x = ui->mouse_x;
	ui->last_mouse_y = ui->mouse_y;
	ui->last_inside = ui->inside;
	ui->last_focused = ui->focused;

	if (ui->pressed || ui->released)
		want = 1;
	if (want)
		ui->motion_pending = 0;
	return want;
}

void ngui_end_ex(struct ngui *ui, int present)
{
	if (present && u_win_present() != 0)
		ui->dirty = 1;

	if (ui->clicked && !ui->focus_claimed)
		ui->focus = 0;
	/* Only the release edge ends a press.  The sampled button state is not
	 * synchronized with the event queue: it can read 0 for a frame before
	 * the release event is delivered.  Clearing active on "!buttons" made
	 * clicks fire only when the two happened to land in the same frame. */
	if (ui->released)
		ui->active = 0;

	ui->pressed = ui->released = 0;
	ui->clicked = 0;
	ui->focus_claimed = 0;
	ui->press_x = ui->press_y = 0;
	ui->release_x = ui->release_y = 0;
}

void ngui_end(struct ngui *ui)
{
	ngui_end_ex(ui, 1);
}

struct ngui_rect ngui_cut_top(struct ngui_rect *r, int h)
{
	struct ngui_rect out = *r;

	if (h < 0)
		h = 0;
	if (h > r->h)
		h = r->h;
	out.h = h;
	r->y += h;
	r->h -= h;
	return out;
}

struct ngui_rect ngui_cut_bottom(struct ngui_rect *r, int h)
{
	struct ngui_rect out = *r;

	if (h < 0)
		h = 0;
	if (h > r->h)
		h = r->h;
	out.y = r->y + r->h - h;
	out.h = h;
	r->h -= h;
	return out;
}

struct ngui_rect ngui_cut_left(struct ngui_rect *r, int w)
{
	struct ngui_rect out = *r;

	if (w < 0)
		w = 0;
	if (w > r->w)
		w = r->w;
	out.w = w;
	r->x += w;
	r->w -= w;
	return out;
}

struct ngui_rect ngui_cut_right(struct ngui_rect *r, int w)
{
	struct ngui_rect out = *r;

	if (w < 0)
		w = 0;
	if (w > r->w)
		w = r->w;
	out.x = r->x + r->w - w;
	out.w = w;
	r->w -= w;
	return out;
}

void ngui_inset(struct ngui_rect *r, int n)
{
	r->x += n;
	r->y += n;
	r->w -= n * 2;
	r->h -= n * 2;
	if (r->w < 0)
		r->w = 0;
	if (r->h < 0)
		r->h = 0;
}

void ngui_push_clip(struct ngui *ui, struct ngui_rect r)
{
	struct ngui_rect out;
	struct ngui_rect canvas;

	if (ui->nclip >= 4)
		return;
	canvas = rect_make(0, 0, ui->w, ui->h);
	if (!rect_intersect(&r, &canvas, &out))
		out = rect_make(0, 0, 0, 0);
	if (ui->nclip > 0) {
		struct ngui_rect clipped = out;

		if (!rect_intersect(&clipped, &ui->clip[ui->nclip - 1], &out))
			out = rect_make(0, 0, 0, 0);
	}
	ui->clip[ui->nclip++] = out;
}

void ngui_pop_clip(struct ngui *ui)
{
	if (ui->nclip > 0)
		ui->nclip--;
}

static int pixel_visible(struct ngui *ui, int x, int y)
{
	struct ngui_rect *c;

	if (x < 0 || y < 0 || x >= ui->w || y >= ui->h)
		return 0;
	if (ui->nclip <= 0)
		return 1;
	c = &ui->clip[ui->nclip - 1];
	return x >= c->x && y >= c->y &&
	       x < c->x + c->w && y < c->y + c->h;
}

void ngui_fill_rect(struct ngui *ui, struct ngui_rect r, unsigned short color)
{
	struct ngui_rect canvas;
	int x, y;

	if (rect_empty(&r))
		return;
	canvas = rect_make(0, 0, ui->w, ui->h);
	if (!rect_intersect(&r, &canvas, &r))
		return;
	if (ui->nclip > 0 &&
	    !rect_intersect(&r, &ui->clip[ui->nclip - 1], &r))
		return;
	for (y = r.y; y < r.y + r.h; y++) {
		unsigned short *p = ui->px + y * ui->w + r.x;

		for (x = 0; x < r.w; x++)
			p[x] = color;
	}
}

void ngui_frame(struct ngui *ui, struct ngui_rect r, unsigned short color)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	ngui_fill_rect(ui, rect_make(r.x, r.y, r.w, 1), color);
	ngui_fill_rect(ui, rect_make(r.x, r.y + r.h - 1, r.w, 1), color);
	if (r.h > 2) {
		ngui_fill_rect(ui, rect_make(r.x, r.y + 1, 1, r.h - 2), color);
		ngui_fill_rect(ui, rect_make(r.x + r.w - 1, r.y + 1, 1,
					     r.h - 2), color);
	}
}

static int ngui_isqrt(int v)
{
	int r = 0, bit = 1 << 30;

	if (v <= 0)
		return 0;
	/* Classic shift-and-subtract sqrt: bit must be the highest power
	 * of four that fits in v, and each step consumes two bits. */
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* Horizontal inset of one scanline of a corner with the given radius. */
static int round_inset(int y, int h, int r)
{
	int dy, d2;

	if (r <= 0)
		return 0;
	if (y < r)
		dy = r - y - 1;
	else if (y >= h - r)
		dy = y - (h - r);
	else
		return 0;
	d2 = r * r - dy * dy;
	if (d2 < 0)
		d2 = 0;
	return r - ngui_isqrt(d2);
}

void ngui_fill_round_rect(struct ngui *ui, struct ngui_rect r,
			  int tl, int tr, int br, int bl,
			  unsigned short color)
{
	int y, half;

	if (rect_empty(&r))
		return;
	half = r.w < r.h ? r.w / 2 : r.h / 2;
	if (tl > half) tl = half;
	if (tr > half) tr = half;
	if (br > half) br = half;
	if (bl > half) bl = half;
	if (tl <= 0 && tr <= 0 && br <= 0 && bl <= 0) {
		ngui_fill_rect(ui, r, color);
		return;
	}
	for (y = 0; y < r.h; y++) {
		int li = 0, ri = 0;

		if (y < tl)
			li = round_inset(y, r.h, tl);
		if (y < tr)
			ri = round_inset(y, r.h, tr);
		if (y >= r.h - bl) {
			int i = round_inset(y, r.h, bl);

			if (i > li)
				li = i;
		}
		if (y >= r.h - br) {
			int i = round_inset(y, r.h, br);

			if (i > ri)
				ri = i;
		}
		if (r.w - li - ri > 0)
			ngui_fill_rect(ui, rect_make(r.x + li, r.y + y,
					     r.w - li - ri, 1), color);
	}
}

void ngui_frame_round(struct ngui *ui, struct ngui_rect r,
		      int tl, int tr, int br, int bl, int thickness,
		      unsigned short color)
{
	int y, half;

	if (rect_empty(&r))
		return;
	if (thickness < 1)
		thickness = 1;
	half = r.w < r.h ? r.w / 2 : r.h / 2;
	if (tl > half) tl = half;
	if (tr > half) tr = half;
	if (br > half) br = half;
	if (bl > half) bl = half;
	if (tl <= 0 && tr <= 0 && br <= 0 && bl <= 0) {
		ngui_frame(ui, r, color);
		return;
	}
	for (y = 0; y < r.h; y++) {
		int li = 0, ri = 0;
		int x0, x1;

		if (y < tl)
			li = round_inset(y, r.h, tl);
		if (y < tr)
			ri = round_inset(y, r.h, tr);
		if (y >= r.h - bl) {
			int i = round_inset(y, r.h, bl);

			if (i > li)
				li = i;
		}
		if (y >= r.h - br) {
			int i = round_inset(y, r.h, br);

			if (i > ri)
				ri = i;
		}
		x0 = r.x + li;
		x1 = r.x + r.w - 1 - ri;
		if (x0 > x1)
			continue;
		if (y < thickness || y >= r.h - thickness) {
			ngui_fill_rect(ui, rect_make(x0, r.y + y,
					     x1 - x0 + 1, 1), color);
		} else {
			ngui_fill_rect(ui, rect_make(x0, r.y + y,
					     thickness, 1), color);
			ngui_fill_rect(ui, rect_make(x1 - thickness + 1,
					     r.y + y, thickness, 1), color);
		}
	}
}

static void ngui_pixel(struct ngui *ui, int x, int y, unsigned short color)
{
	if (pixel_visible(ui, x, y))
		ui->px[y * ui->w + x] = color;
}

void ngui_line(struct ngui *ui, int x0, int y0, int x1, int y1,
	       unsigned short color)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int sx = dx < 0 ? -1 : 1;
	int sy = dy < 0 ? -1 : 1;
	int err, e2;

	if (dx < 0)
		dx = -dx;
	if (dy < 0)
		dy = -dy;
	dy = -dy;
	err = dx + dy;
	for (;;) {
		ngui_pixel(ui, x0, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = err * 2;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void ngui_glyph(struct ngui *ui, int x, int y, unsigned char c,
		       unsigned short color, int scale, int bold)
{
	const unsigned char *g = &font_8x16[(unsigned int)c * FONT_8X16_ROWS];
	int row, col, sx, sy;

	if (scale < 1)
		scale = 1;
	for (row = 0; row < FONT_8X16_ROWS; row++) {
		unsigned char bits = g[row];

		if (!bits)
			continue;
		for (col = 0; col < 8; col++) {
			if (!(bits & (0x80u >> col)))
				continue;
			for (sy = 0; sy < scale; sy++) {
				for (sx = 0; sx < scale; sx++) {
					ngui_pixel(ui,
						   x + col * scale + sx +
							(bold && scale == 1 ? 1 : 0),
						   y + row * scale + sy,
						   color);
				}
			}
			if (bold && scale > 1)
				ngui_pixel(ui, x + col * scale + scale,
					   y + row * scale, color);
		}
	}
}

unsigned int ngui_text_width(const char *s)
{
	return slen(s) * 8;
}

unsigned int ngui_text_width_n(const char *s, unsigned int n, int scale)
{
	(void)s;
	if (scale < 1)
		scale = 1;
	return n * 8u * (unsigned int)scale;
}

void ngui_text_n(struct ngui *ui, int x, int y, const char *s, unsigned int n,
		 unsigned short color)
{
	unsigned int i;

	for (i = 0; i < n && s && s[i]; i++)
		ngui_glyph(ui, x + (int)i * 8, y, (unsigned char)s[i], color, 1, 0);
}

void ngui_text(struct ngui *ui, int x, int y, const char *s,
	       unsigned short color)
{
	ngui_text_n(ui, x, y, s, slen(s), color);
}

void ngui_text_styled(struct ngui *ui, int x, int y, const char *s,
		      unsigned int n, unsigned short color, int scale, int bold)
{
	unsigned int i;

	if (scale < 1)
		scale = 1;
	for (i = 0; i < n && s && s[i]; i++)
		ngui_glyph(ui, x + (int)i * 8 * scale, y, (unsigned char)s[i],
			   color, scale, bold);
}

/* Draw a string upside down.  The whole n-character block is rotated, so the
 * first character ends up on the right; used for the mirrored corner index on
 * a playing card. */
void ngui_text_rot180(struct ngui *ui, int x, int y, const char *s,
		      unsigned int n, unsigned short color)
{
	unsigned int i;
	int row, col;

	for (i = 0; i < n && s && s[i]; i++) {
		const unsigned char *g =
			&font_8x16[(unsigned int)(unsigned char)s[i] *
				   FONT_8X16_ROWS];

		for (row = 0; row < FONT_8X16_ROWS; row++) {
			unsigned char bits = g[row];

			if (!bits)
				continue;
			for (col = 0; col < 8; col++) {
				if (!(bits & (0x80u >> col)))
					continue;
				ngui_pixel(ui,
					   x + (int)(n - 1 - i) * 8 + 7 - col,
					   y + FONT_8X16_ROWS - 1 - row,
					   color);
			}
		}
	}
}

void ngui_text_ellipsis(struct ngui *ui, struct ngui_rect r, int x, int y,
			const char *s, unsigned short color)
{
	unsigned int n = slen(s);
	int max = (r.x + r.w - x) / 8;
	unsigned int i;

	if (max <= 0)
		return;
	if (n <= (unsigned int)max || max < 4) {
		for (i = 0; i < n && (int)i < max; i++)
			ngui_glyph(ui, x + (int)i * 8, y,
				   (unsigned char)s[i], color, 1, 0);
		return;
	}
	for (i = 0; i < (unsigned int)(max - 3); i++)
		ngui_glyph(ui, x + (int)i * 8, y, (unsigned char)s[i], color, 1, 0);
	ngui_glyph(ui, x + (max - 3) * 8, y, '.', color, 1, 0);
	ngui_glyph(ui, x + (max - 2) * 8, y, '.', color, 1, 0);
	ngui_glyph(ui, x + (max - 1) * 8, y, '.', color, 1, 0);
}

void ngui_icon(struct ngui *ui, struct ngui_rect r, enum ngui_icon icon,
	       unsigned short color)
{
	int x = r.x + r.w / 2 - 8;
	int y = r.y + r.h / 2 - 8;

	switch (icon) {
	case NGUI_ICON_FOLDER:
		ngui_fill_rect(ui, rect_make(x + 1, y + 3, 7, 4), color);
		ngui_fill_rect(ui, rect_make(x, y + 6, 16, 9), color);
		break;
	case NGUI_ICON_FILE:
		ngui_frame(ui, rect_make(x + 2, y, 12, 16), color);
		ngui_line(ui, x + 10, y, x + 13, y + 3, color);
		ngui_fill_rect(ui, rect_make(x + 4, y + 7, 8, 1), color);
		ngui_fill_rect(ui, rect_make(x + 4, y + 10, 8, 1), color);
		break;
	case NGUI_ICON_DISK:
		ngui_frame(ui, rect_make(x, y + 2, 16, 12), color);
		ngui_fill_rect(ui, rect_make(x + 2, y + 5, 12, 2), color);
		ngui_fill_rect(ui, rect_make(x + 11, y + 9, 3, 3), color);
		break;
	case NGUI_ICON_UP:
		ngui_line(ui, x + 8, y + 2, x + 3, y + 8, color);
		ngui_line(ui, x + 8, y + 2, x + 13, y + 8, color);
		ngui_fill_rect(ui, rect_make(x + 6, y + 7, 5, 7), color);
		break;
	case NGUI_ICON_HOME:
		ngui_line(ui, x + 2, y + 8, x + 8, y + 2, color);
		ngui_line(ui, x + 8, y + 2, x + 14, y + 8, color);
		ngui_frame(ui, rect_make(x + 3, y + 8, 10, 7), color);
		ngui_fill_rect(ui, rect_make(x + 7, y + 11, 3, 4), color);
		break;
	case NGUI_ICON_REFRESH:
		ngui_line(ui, x + 3, y + 8, x + 6, y + 4, color);
		ngui_line(ui, x + 6, y + 4, x + 10, y + 3, color);
		ngui_line(ui, x + 10, y + 3, x + 13, y + 6, color);
		ngui_line(ui, x + 13, y + 6, x + 13, y + 9, color);
		ngui_fill_rect(ui, rect_make(x + 11, y + 1, 4, 4), color);
		break;
	case NGUI_ICON_PLUS:
		ngui_fill_rect(ui, rect_make(x + 7, y + 3, 2, 10), color);
		ngui_fill_rect(ui, rect_make(x + 3, y + 7, 10, 2), color);
		break;
	case NGUI_ICON_CHEVRON_LEFT:
		ngui_line(ui, x + 10, y + 3, x + 6, y + 8, color);
		ngui_line(ui, x + 6, y + 8, x + 10, y + 13, color);
		break;
	case NGUI_ICON_CHEVRON_RIGHT:
		ngui_line(ui, x + 6, y + 3, x + 10, y + 8, color);
		ngui_line(ui, x + 10, y + 8, x + 6, y + 13, color);
		break;
	default:
		break;
	}
}

int ngui_button(struct ngui *ui, unsigned int id, struct ngui_rect r,
		const char *label)
{
	int hot = ui->inside && ngui_rect_contains(&r, ui->mouse_x, ui->mouse_y);
	int press_hot = ui->pressed &&
		ngui_rect_contains(&r, ui->press_x, ui->press_y);
	int release_hot = ngui_rect_contains(&r, ui->release_x, ui->release_y);
	int fired = 0;
	unsigned short bg;
	int tw, tx, ty;

	if (press_hot)
		ui->active = id;
	if (ui->pressed && ui->released && press_hot && release_hot)
		fired = 1;
	if (ui->released && ui->active == id) {
		if (release_hot)
			fired = 1;
		ui->active = 0;
	}

	if (ui->active == id)
		bg = ui->theme.button_down;
	else if (hot)
		bg = ui->theme.button_hot;
	else
		bg = ui->theme.button;
	ngui_fill_rect(ui, r, bg);
	ngui_frame(ui, r, hot ? ui->theme.accent : ui->theme.border);

	if (label && label[0]) {
		tw = (int)ngui_text_width(label);
		tx = r.x + (r.w - tw) / 2;
		ty = r.y + (r.h - 16) / 2;
		ngui_push_clip(ui, r);
		ngui_text(ui, tx, ty, label, ui->theme.text);
		ngui_pop_clip(ui);
	}
	if (hot)
		ui->hot = id;
	return fired;
}

/* One row of a drop-down menu: left-aligned label, optional check mark and
 * right-aligned shortcut.  Uses the same press/release/active semantics as
 * ngui_button so a menu and its caller never disagree about a click. */
int ngui_menu_item(struct ngui *ui, unsigned int id, struct ngui_rect r,
		   const char *label, const char *shortcut, int checked)
{
	int hot = ui->inside && ngui_rect_contains(&r, ui->mouse_x, ui->mouse_y);
	int press_hot = ui->pressed &&
		ngui_rect_contains(&r, ui->press_x, ui->press_y);
	int release_hot = ngui_rect_contains(&r, ui->release_x, ui->release_y);
	int fired = 0;
	int selected;
	unsigned short bg, fg;
	int ty;

	if (press_hot)
		ui->active = id;
	if (ui->pressed && ui->released && press_hot && release_hot)
		fired = 1;
	if (ui->released && ui->active == id) {
		if (release_hot)
			fired = 1;
		ui->active = 0;
	}

	selected = (ui->active == id) || hot;
	bg = selected ? ui->theme.selection : ui->theme.panel;
	fg = selected ? NGUI_RGB(255, 255, 255) : ui->theme.text;
	ngui_fill_rect(ui, r, bg);

	ty = r.y + (r.h - 16) / 2;
	ngui_push_clip(ui, r);
	if (checked) {
		ngui_line(ui, r.x + 8, ty + 10, r.x + 10, ty + 13, fg);
		ngui_line(ui, r.x + 10, ty + 13, r.x + 15, ty + 5, fg);
	}
	if (label)
		ngui_text(ui, r.x + 22, ty, label, fg);
	if (shortcut) {
		int sw = (int)ngui_text_width(shortcut);

		ngui_text(ui, r.x + r.w - 8 - sw, ty, shortcut, fg);
	}
	ngui_pop_clip(ui);
	if (hot)
		ui->hot = id;
	return fired;
}

int ngui_icon_button(struct ngui *ui, unsigned int id, struct ngui_rect r,
		     enum ngui_icon icon){
	int hot = ui->inside && ngui_rect_contains(&r, ui->mouse_x, ui->mouse_y);
	int press_hot = ui->pressed &&
		ngui_rect_contains(&r, ui->press_x, ui->press_y);
	int release_hot = ngui_rect_contains(&r, ui->release_x, ui->release_y);
	int fired = 0;
	unsigned short bg;

	if (press_hot)
		ui->active = id;
	if (ui->pressed && ui->released && press_hot && release_hot)
		fired = 1;
	if (ui->released && ui->active == id) {
		if (release_hot)
			fired = 1;
		ui->active = 0;
	}

	if (ui->active == id)
		bg = ui->theme.button_down;
	else if (hot)
		bg = ui->theme.button_hot;
	else
		bg = ui->theme.button;
	ngui_fill_rect(ui, r, bg);
	ngui_frame(ui, r, hot ? ui->theme.accent : ui->theme.border);
	ngui_icon(ui, r, icon, ui->theme.text);
	if (hot)
		ui->hot = id;
	return fired;
}

void ngui_input_set(struct ngui_input *in, const char *s)
{
	scopy(in->text, s, NGUI_INPUT_MAX);
	in->len = slen(in->text);
	in->cursor = in->len;
	in->scroll = 0;
	in->anchor = in->cursor;
	in->dragging = 0;
	in->last_click_ms = 0;
}

static int input_word_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

/* Next word start after `pos` (Windows edit-control semantics). */
static unsigned int input_word_next(const struct ngui_input *in,
				    unsigned int pos)
{
	while (pos < in->len && input_word_char(in->text[pos]))
		pos++;
	while (pos < in->len && !input_word_char(in->text[pos]))
		pos++;
	return pos;
}

/* Start of the previous word before `pos`. */
static unsigned int input_word_prev(const struct ngui_input *in,
				    unsigned int pos)
{
	while (pos > 0 && !input_word_char(in->text[pos - 1]))
		pos--;
	while (pos > 0 && input_word_char(in->text[pos - 1]))
		pos--;
	return pos;
}

/* End of the word to the right of `pos`.  A caret inside a word takes the
 * rest of that word and its trailing separators; a caret on separators takes
 * them plus the next word. */
static unsigned int input_word_delete_end(const struct ngui_input *in,
					  unsigned int pos)
{
	if (pos < in->len && input_word_char(in->text[pos])) {
		while (pos < in->len && input_word_char(in->text[pos]))
			pos++;
		while (pos < in->len && !input_word_char(in->text[pos]))
			pos++;
		return pos;
	}
	while (pos < in->len && !input_word_char(in->text[pos]))
		pos++;
	while (pos < in->len && input_word_char(in->text[pos]))
		pos++;
	return pos;
}

static void input_sel_bounds(const struct ngui_input *in,
			     unsigned int *a, unsigned int *b)
{
	if (in->anchor < in->cursor) {
		*a = in->anchor;
		*b = in->cursor;
	} else {
		*a = in->cursor;
		*b = in->anchor;
	}
}

/* Remove [a, b) and keep cursor/anchor consistent.  Returns 1 when the text
 * changed. */
static int input_delete_range(struct ngui_input *in, unsigned int a,
			      unsigned int b)
{
	unsigned int i;

	if (a >= b || b > in->len)
		return 0;
	for (i = b; i < in->len; i++)
		in->text[i - (b - a)] = in->text[i];
	in->len -= b - a;
	in->text[in->len] = '\0';
	if (in->cursor >= b)
		in->cursor -= b - a;
	else if (in->cursor > a)
		in->cursor = a;
	if (in->anchor >= b)
		in->anchor -= b - a;
	else if (in->anchor > a)
		in->anchor = a;
	return 1;
}

static int input_delete_selection(struct ngui_input *in)
{
	unsigned int a, b;

	input_sel_bounds(in, &a, &b);
	return input_delete_range(in, a, b);
}

static void input_insert(struct ngui_input *in, char c)
{
	unsigned int i;

	input_delete_selection(in);
	if (in->len + 1 >= NGUI_INPUT_MAX)
		return;
	i = in->len;
	while (i > in->cursor) {
		in->text[i] = in->text[i - 1];
		i--;
	}
	in->text[in->cursor] = c;
	in->cursor++;
	in->len++;
	in->text[in->len] = '\0';
	in->anchor = in->cursor;
}

static void input_delete(struct ngui_input *in, int back)
{
	unsigned int pos;

	if (back) {
		if (in->cursor == 0)
			return;
		pos = in->cursor - 1;
	} else {
		if (in->cursor >= in->len)
			return;
		pos = in->cursor;
	}
	input_delete_range(in, pos, pos + 1);
}

static void input_move_to(struct ngui_input *in, unsigned int pos)
{
	if (pos > in->len)
		pos = in->len;
	in->cursor = pos;
	in->anchor = pos;
}

/* Two presses in the same field within this window select all of its text. */
#define NGUI_DOUBLE_CLICK_MS 500

/* Character position under a content-relative x, clamped to the visible
 * part of the field. */
static unsigned int input_pos_at(const struct ngui_input *in, int inner_x,
				 int cols, int x)
{
	int col = (x - inner_x) / 8;
	unsigned int pos;

	if (col < 0)
		col = 0;
	if (cols > 0 && col > cols)
		col = cols;
	pos = in->scroll + (unsigned int)col;
	if (pos > in->len)
		pos = in->len;
	return pos;
}

static void input_draw_selection(struct ngui *ui, const struct ngui_input *in,
				 struct ngui_rect inner, int cols)
{
	unsigned int a, b, va, vb;

	input_sel_bounds(in, &a, &b);
	if (a == b || cols <= 0)
		return;
	va = a < in->scroll ? in->scroll : a;
	vb = b > in->scroll + (unsigned int)cols
	     ? in->scroll + (unsigned int)cols : b;
	if (va < vb)
		ngui_fill_rect(ui, rect_make(inner.x + (int)(va - in->scroll) * 8,
					     inner.y, (int)(vb - va) * 8, 16),
			       ui->theme.selection);
}

int ngui_text_input(struct ngui *ui, unsigned int id, struct ngui_rect r,
		    struct ngui_input *in)
{
	struct ngui_rect inner;
	int focused = (ui->focus == id);
	int result = 0;
	int cols, k;
	int changed = 0;

	inner = rect_make(r.x + 4, r.y + (r.h - 16) / 2, r.w - 8, 16);
	if (inner.w < 0)
		inner.w = 0;
	cols = inner.w / 8;
	if (cols < 0)
		cols = 0;

	if (ui->pressed &&
	    ngui_rect_contains(&r, ui->press_x, ui->press_y)) {
		unsigned int now = u_ticks_ms();
		int dbl = in->last_click_ms &&
			  (unsigned int)(now - in->last_click_ms) <=
			  NGUI_DOUBLE_CLICK_MS;

		if (dbl) {
			/* Double click: select the whole text and keep it
			 * selected while the second press is held. */
			in->anchor = 0;
			in->cursor = in->len;
			in->dragging = 0;
		} else {
			unsigned int pos = input_pos_at(in, inner.x, cols,
							ui->press_x);

			in->cursor = pos;
			in->anchor = pos;
			in->dragging = 1;
		}
		in->last_click_ms = now;
		ui->focus = id;
		ui->focus_claimed = 1;
		ui->last_blink = now;
		ui->caret_on = 1;
		focused = 1;
	} else if (ui->pressed) {
		in->dragging = 0;
	}

	/* Extend the selection while the button that started in this field is
	 * still held.  Sampled coordinates may be outside the canvas.  The
	 * release *event* is authoritative: the sampled button state can lag
	 * the event queue by a frame. */
	if (in->dragging && (ui->buttons & 1) && !ui->released) {
		in->cursor = input_pos_at(in, inner.x, cols, ui->mouse_x);
		/* Keep redrawing when the pointer leaves the window. */
		ngui_invalidate(ui);
	}
	if (ui->released)
		in->dragging = 0;

	if (focused) {
		while ((k = ngui_key(ui)) != 0) {
			if (k == '\n' || k == '\r') {
				result |= NGUI_INPUT_SUBMIT;
				break;
			}
			if (k == NGUI_KEY_ESC || k == NGUI_KEY_TAB) {
				result |= NGUI_INPUT_CANCEL;
				ui->focus = 0;
				break;
			}
			if (k == NGUI_KEY_BACKSPACE || k == 0x7F) {
				if (!input_delete_selection(in))
					input_delete(in, 1);
				changed = 1;
			} else if (k == NGUI_KEY_DELETE) {
				if (!input_delete_selection(in))
					input_delete(in, 0);
				changed = 1;
			} else if (k == NGUI_KEY_CTRL_BACKSPACE) {
				if (input_delete_selection(in))
					changed = 1;
				else
					changed = input_delete_range(in,
						input_word_prev(in, in->cursor),
						in->cursor);
			} else if (k == NGUI_KEY_CTRL_DELETE) {
				if (input_delete_selection(in)) {
					changed = 1;
				} else {
					unsigned int a = in->cursor;
					unsigned int b = input_word_delete_end(in,
									      a);

					/* At the end of the line, delete the word
					 * before the caret instead of doing
					 * nothing. */
					if (b == a) {
						a = input_word_prev(in, in->cursor);
						b = in->cursor;
					}
					changed = input_delete_range(in, a, b);
				}
			} else if (k == NGUI_KEY_LEFT) {
				input_move_to(in, in->cursor > 0 ? in->cursor - 1 : 0);
			} else if (k == NGUI_KEY_RIGHT) {
				input_move_to(in, in->cursor < in->len
						  ? in->cursor + 1 : in->len);
			} else if (k == NGUI_KEY_CTRL_LEFT) {
				input_move_to(in, input_word_prev(in, in->cursor));
			} else if (k == NGUI_KEY_CTRL_RIGHT) {
				input_move_to(in, input_word_next(in, in->cursor));
			} else if (k == NGUI_KEY_HOME) {
				input_move_to(in, 0);
			} else if (k == NGUI_KEY_END) {
				input_move_to(in, in->len);
			} else if (k == 0x01) {   /* Ctrl+A: select all */
				in->anchor = 0;
				in->cursor = in->len;
			} else if (k == 0x15) {   /* Ctrl+U */
				in->len = 0;
				in->cursor = 0;
				in->anchor = 0;
				in->text[0] = '\0';
				changed = 1;
			} else if (k >= 0x20 && k <= 0x7E) {
				input_insert(in, (char)k);
				changed = 1;
			}
		}
	}

	ngui_fill_rect(ui, r, focused ? ui->theme.panel_alt : ui->theme.panel);
	ngui_frame(ui, r, focused ? ui->theme.accent : ui->theme.border);

	if (in->scroll > in->len)
		in->scroll = in->len;
	if (in->cursor < in->scroll)
		in->scroll = in->cursor;
	if (cols > 0 && in->cursor >= in->scroll + (unsigned int)cols)
		in->scroll = in->cursor - (unsigned int)cols + 1;
	if (in->scroll > in->len)
		in->scroll = in->len;

	ngui_push_clip(ui, inner);
	if (in->len == 0 && in->placeholder) {
		ngui_text(ui, inner.x, inner.y, in->placeholder,
			  ui->theme.text_dim);
	} else {
		if (focused)
			input_draw_selection(ui, in, inner, cols);
		ngui_text(ui, inner.x, inner.y, in->text + in->scroll,
			  ui->theme.text);
	}

	if (focused && ui->caret_on) {
		int col = (int)in->cursor - (int)in->scroll;

		if (col >= 0 && col <= cols)
			ngui_fill_rect(ui, rect_make(inner.x + col * 8, inner.y,
						     1, 16), ui->theme.accent);
	}
	ngui_pop_clip(ui);

	if (changed)
		result |= NGUI_INPUT_CHANGED;
	return result;
}

static int scrollable(const struct ngui_scroll *s)
{
	return s->content_h > s->view_h && s->view_h > 0;
}

static int scroll_thumb_h(const struct ngui_rect *r,
			  const struct ngui_scroll *s)
{
	int h = 0;

	if (s->content_h > 0)
		h = r->h * s->view_h / s->content_h;
	if (h < 12)
		h = 12;
	if (h > r->h)
		h = r->h;
	return h;
}

static int scroll_thumb_y(const struct ngui_rect *r,
			  const struct ngui_scroll *s)
{
	int th = scroll_thumb_h(r, s);
	int avail = r->h - th;
	int max = s->content_h - s->view_h;

	if (avail <= 0 || max <= 0)
		return r->y;
	return r->y + s->offset * avail / max;
}

static void scroll_clamp(struct ngui_scroll *s)
{
	int max = s->content_h - s->view_h;

	if (max < 0)
		max = 0;
	if (s->offset > max)
		s->offset = max;
	if (s->offset < 0)
		s->offset = 0;
}

void ngui_scroll_to(struct ngui_scroll *s, int offset)
{
	s->offset = offset;
	scroll_clamp(s);
}

void ngui_scroll_ensure_visible(struct ngui_scroll *s, int y, int h)
{
	if (y < s->offset)
		s->offset = y;
	else if (y + h > s->offset + s->view_h)
		s->offset = y + h - s->view_h;
	scroll_clamp(s);
}

void ngui_scroll_begin(struct ngui *ui, struct ngui_rect r,
		       struct ngui_scroll *s)
{
	struct ngui_rect bar;
	int scrolls = scrollable(s);

	s->view_h = r.h;
	scroll_clamp(s);
	if (!ui->buttons)
		s->dragging = 0;
	if (scrolls) {
		int th = scroll_thumb_h(&r, s);
		int ty = scroll_thumb_y(&r, s);

		bar = rect_make(r.x + r.w - 15, r.y, 14, r.h);
		if (ui->pressed &&
		    ngui_rect_contains(&bar, ui->press_x, ui->press_y)) {
			if (ui->press_y >= ty && ui->press_y < ty + th) {
				s->dragging = 1;
				s->drag_dy = ui->press_y - ty;
			} else {
				int rel = ui->press_y - r.y - th / 2;
				int avail = r.h - th;
				int max = s->content_h - s->view_h;

				if (rel < 0)
					rel = 0;
				if (rel > avail)
					rel = avail;
				if (avail > 0)
					s->offset = rel * max / avail;
				s->dragging = 1;
				s->drag_dy = th / 2;
				scroll_clamp(s);
			}
		}
		/* Sampled buttons are authoritative while held: some event paths
		 * deliver the press late or coalesce it with the release.  Start
		 * the drag from the current pointer position as well. */
		if (!s->dragging && (ui->buttons & 1) &&
		    ngui_rect_contains(&bar, ui->mouse_x, ui->mouse_y)) {
			if (ui->mouse_y >= ty && ui->mouse_y < ty + th) {
				s->dragging = 1;
				s->drag_dy = ui->mouse_y - ty;
			} else {
				int rel = ui->mouse_y - r.y - th / 2;
				int avail = r.h - th;
				int max = s->content_h - s->view_h;

				if (rel < 0)
					rel = 0;
				if (rel > avail)
					rel = avail;
				if (avail > 0)
					s->offset = rel * max / avail;
				s->dragging = 1;
				s->drag_dy = th / 2;
				scroll_clamp(s);
			}
		}
		if (s->dragging && (ui->buttons & 1)) {
			int rel = ui->mouse_y - r.y - s->drag_dy;
			int avail = r.h - th;
			int max = s->content_h - s->view_h;

			if (rel < 0)
				rel = 0;
			if (rel > avail)
				rel = avail;
			if (avail > 0)
				s->offset = rel * max / avail;
			scroll_clamp(s);
		}
		if (ui->released)
			s->dragging = 0;
	}
	ngui_push_clip(ui, r);
}

void ngui_scroll_end(struct ngui *ui, struct ngui_rect r,
		     struct ngui_scroll *s)
{
	ngui_pop_clip(ui);
	if (!scrollable(s))
		return;
	{
		struct ngui_rect bar = rect_make(r.x + r.w - 15, r.y, 14, r.h);
		int th = scroll_thumb_h(&r, s);
		int ty = scroll_thumb_y(&r, s);
		struct ngui_rect thumb = rect_make(bar.x, ty, 14, th);

		ngui_fill_rect(ui, bar, ui->theme.panel_alt);
		ngui_fill_rect(ui, thumb, s->dragging ?
			       ui->theme.accent : ui->theme.text_dim);
	}
}

int ngui_scroll_keys(struct ngui *ui, struct ngui_scroll *s)
{
	int k = ngui_key_peek(ui);
	int page;

	if (k != NGUI_KEY_PAGE_UP && k != NGUI_KEY_PAGE_DOWN)
		return 0;
	page = s->view_h - 22;
	if (page < 22)
		page = 22;
	ngui_key(ui);
	if (k == NGUI_KEY_PAGE_UP)
		s->offset -= page;
	else
		s->offset += page;
	scroll_clamp(s);
	return 1;
}
