/* view: image viewer built on the img library and the ngui toolkit.
 *
 * Usage: view <file>.  The command owns a canvas window, reads the whole file
 * through the kernel file service, decodes it into an exact-size malloc arena,
 * and draws it with fit/1:1/zoom/pan support. */

#include <stdlib.h>

#include "ucmd_lib.h"
#include "ngui.h"
#include "img.h"
#include "img_file.h"

#define VIEW_FILE_CAP   (IMG_FILE_MAX + 1u)
#define VIEW_HEAP_MAX   (32u * 1024u * 1024u)
#define VIEW_TOOLBAR_H  28
#define VIEW_STATUS_H   18
#define VIEW_BAR_H      (VIEW_TOOLBAR_H + VIEW_STATUS_H)

#define VIEW_ID_FIT    1
#define VIEW_ID_ONE    2
#define VIEW_ID_OUT    3
#define VIEW_ID_IN     4

#define VIEW_BG        NGUI_RGB(24, 26, 32)
#define VIEW_VIEW_BG   NGUI_RGB(16, 17, 21)
#define VIEW_CHK1      NGUI_RGB(46, 48, 54)
#define VIEW_CHK2      NGUI_RGB(58, 60, 67)

struct viewer {
	struct ngui ui;
	struct img_image img;
	struct img_mem mem;
	unsigned char *file;
	unsigned char *arena;

	unsigned int file_size;
	unsigned int iw, ih;
	unsigned int format;
	unsigned int zoom;          /* 16.16 when fit == 0 */
	int fit;
	int off_x, off_y;           /* destination top-left inside the viewport */
	int dragging;
	int drag_x, drag_y;
	int have_img;
	int loading;
	int error;
	unsigned int gif_frames, gif_index, gif_delay, gif_next_ms;
	int last_w, last_h;
	char err[64];
	char name[20];
	char title[20];
	int dirty;
};

static struct viewer v;

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void s_copy(char *d, const char *s, unsigned int cap)
{
	unsigned int i = 0;

	if (!cap)
		return;
	while (s && s[i] && i + 1 < cap) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
}

static void s_append(char *d, unsigned int *n, const char *s, unsigned int cap)
{
	while (s && *s && *n + 1 < cap)
		d[(*n)++] = *s++;
	d[*n] = '\0';
}

static void s_append_uint(char *d, unsigned int *n, unsigned int v,
			  unsigned int cap)
{
	char tmp[12];
	unsigned int i = 0, j;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < sizeof(tmp));
	for (j = 0; j < i && *n + 1 < cap; j++)
		d[(*n)++] = tmp[i - 1 - j];
	d[*n] = '\0';
}

static void base_name(const char *path, char *out, unsigned int cap)
{
	unsigned int n = 0, i, start = 0;

	if (!path)
		path = "";
	for (i = 0; path[i]; i++)
		if (path[i] == '/')
			start = i + 1;
	for (i = start; path[i] && n + 1 < cap; i++)
		out[n++] = path[i];
	out[n] = '\0';
	if (!n)
		s_copy(out, "image", cap);
}

struct ngui_rect view_body(struct ngui *ui)
{
	struct ngui_rect r = {0, 0, ui->w, ui->h};
	struct ngui_rect top, bottom;

	top = ngui_cut_top(&r, ui->h >= 140 ? VIEW_TOOLBAR_H : 22);
	bottom = ngui_cut_bottom(&r, ui->h >= 140 ? VIEW_STATUS_H : 14);
	(void)top;
	(void)bottom;
	return r;
}

static unsigned int view_scale(struct ngui *ui)
{
	struct ngui_rect b = view_body(ui);
	unsigned int s;

	if (!v.have_img || b.w <= 0 || b.h <= 0)
		return 65536;
	if (!v.fit)
		return v.zoom ? v.zoom : 65536;
	s = img_fit_scale(v.iw, v.ih, (unsigned int)b.w, (unsigned int)b.h);
	if (s > 65536)
		s = 65536;              /* don't enlarge a smaller image */
	return s ? s : 256;
}

static void clamp_view(struct ngui *ui, unsigned int scale)
{
	struct ngui_rect b = view_body(ui);
	unsigned int sw, sh;

	if (!v.have_img) {
		v.off_x = v.off_y = 0;
		return;
	}
	sw = (unsigned int)(((unsigned long long)v.iw * scale) >> 16);
	sh = (unsigned int)(((unsigned long long)v.ih * scale) >> 16);
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;
	if (sw <= (unsigned int)b.w)
		v.off_x = ((int)b.w - (int)sw) / 2;
	else {
		if (v.off_x > 0) v.off_x = 0;
		if (v.off_x < (int)b.w - (int)sw)
			v.off_x = (int)b.w - (int)sw;
	}
	if (sh <= (unsigned int)b.h)
		v.off_y = ((int)b.h - (int)sh) / 2;
	else {
		if (v.off_y > 0) v.off_y = 0;
		if (v.off_y < (int)b.h - (int)sh)
			v.off_y = (int)b.h - (int)sh;
	}
	if (v.fit) {
		v.off_x = ((int)b.w - (int)sw) / 2;
		v.off_y = ((int)b.h - (int)sh) / 2;
	}
}

static void view_set_scale(struct ngui *ui, unsigned int scale)
{
	struct ngui_rect b = view_body(ui);
	unsigned int old = view_scale(ui);
	unsigned int sx, sy;
	int dx, dy;

	if (!v.have_img || b.w <= 0 || b.h <= 0)
		return;
	if (scale < 256) scale = 256;
	if (scale > 8u * 65536u) scale = 8u * 65536u;
	dx = b.w / 2 - v.off_x;
	dy = b.h / 2 - v.off_y;
	sx = ((unsigned int)dx << 16) / old;
	sy = ((unsigned int)dy << 16) / old;
	v.fit = 0;
	v.zoom = scale;
	v.off_x = (int)(b.w / 2 - (int)(((unsigned long long)sx * scale) >> 16));
	v.off_y = (int)(b.h / 2 - (int)(((unsigned long long)sy * scale) >> 16));
	clamp_view(ui, scale);
}

static void view_fit(void)
{
	v.fit = 1;
}

static void view_one(struct ngui *ui)
{
	struct ngui_rect b = view_body(ui);
	unsigned int sw;

	v.fit = 0;
	v.zoom = 65536;
	sw = v.iw;
	v.off_x = ((int)b.w - (int)sw) / 2;
	v.off_y = ((int)b.h - (int)v.ih) / 2;
	clamp_view(ui, v.zoom);
}

static void view_zoom_step(struct ngui *ui, int in)
{
	unsigned int s = view_scale(ui);

	if (in)
		s = s + s / 4;
	else
		s = s - s / 5;
	view_set_scale(ui, s);
}

static void format_status(char *buf, unsigned int cap)
{
	unsigned int n = 0;

	buf[0] = '\0';
	if (v.have_img) {
		unsigned int scale = view_scale(&v.ui);

		s_append(buf, &n, img_format_name(v.format), cap);
		s_append(buf, &n, "  ", cap);
		s_append_uint(buf, &n, v.iw, cap);
		s_append(buf, &n, "x", cap);
		s_append_uint(buf, &n, v.ih, cap);
		s_append(buf, &n, "  ", cap);
		if (v.file_size >= 1024 * 1024) {
			s_append_uint(buf, &n, v.file_size / (1024 * 1024), cap);
			s_append(buf, &n, " MB", cap);
		} else {
			s_append_uint(buf, &n, (v.file_size + 1023) / 1024, cap);
			s_append(buf, &n, " KB", cap);
		}
		s_append(buf, &n, "  ", cap);
		if (v.fit)
			s_append(buf, &n, "fit ", cap);
		s_append_uint(buf, &n, (scale * 100u) >> 16, cap);
		s_append(buf, &n, "%", cap);
	} else {
		s_append(buf, &n, "view  ", cap);
		s_append(buf, &n, v.name, cap);
	}
}

static void draw_message(struct ngui *ui, struct ngui_rect body,
			 const char *msg)
{
	int tw = (int)ngui_text_width_ui(ui, msg);
	int x = body.x + (body.w - tw) / 2;
	int y = body.y + body.h / 2 - 8;

	if (x < body.x + 6)
		x = body.x + 6;
	ngui_text(ui, x, y, msg, ui->theme.text);
	if (!v.loading) {
		const char *hint = "Esc closes";
		int hw = (int)ngui_text_width_ui(ui, hint);

		ngui_text(ui, body.x + (body.w - hw) / 2, y + 24, hint,
			  ui->theme.text_dim);
	}
}

static void draw_toolbar(struct ngui *ui, struct ngui_rect r)
{
	struct ngui_rect b;
	int x = r.x + 6;
	int by = r.y + (r.h - 20) / 2;
	int bw;

	ngui_fill_rect(ui, r, ui->theme.panel);
	ngui_fill_rect(ui, (struct ngui_rect){r.x, r.y + r.h - 1, r.w, 1},
		       ui->theme.border);

	bw = 40;
	b = (struct ngui_rect){x, by, bw, 20};
	if (ngui_button(ui, VIEW_ID_FIT, b, "Fit")) {
		view_fit();
		v.dirty = 1;
	}
	x += bw + 5;
	b = (struct ngui_rect){x, by, 40, 20};
	if (ngui_button(ui, VIEW_ID_ONE, b, "1:1")) {
		view_one(ui);
		v.dirty = 1;
	}
	x += 45;
	b = (struct ngui_rect){x, by, 24, 20};
	if (ngui_button(ui, VIEW_ID_OUT, b, "-")) {
		view_zoom_step(ui, 0);
		v.dirty = 1;
	}
	x += 29;
	b = (struct ngui_rect){x, by, 24, 20};
	if (ngui_button(ui, VIEW_ID_IN, b, "+")) {
		view_zoom_step(ui, 1);
		v.dirty = 1;
	}
	x += 32;
	if (v.have_img && x + 70 < r.x + r.w) {
		char z[16];
		unsigned int n = 0;

		z[0] = '\0';
		s_append_uint(z, &n, (view_scale(ui) * 100u) >> 16, sizeof(z));
		s_append(z, &n, "%", sizeof(z));
		ngui_text(ui, x, r.y + (r.h - 16) / 2, z, ui->theme.text_dim);
	}
}

static void draw(struct ngui *ui)
{
	struct ngui_rect all = {0, 0, ui->w, ui->h};
	struct ngui_rect r = all;
	struct ngui_rect toolbar, status, body;
	char line[96];

	toolbar = ngui_cut_top(&r, ui->h >= 140 ? VIEW_TOOLBAR_H : 22);
	status = ngui_cut_bottom(&r, ui->h >= 140 ? VIEW_STATUS_H : 14);
	body = r;

	ngui_fill_rect(ui, all, VIEW_BG);
	draw_toolbar(ui, toolbar);
	ngui_fill_rect(ui, status, ui->theme.panel);
	format_status(line, sizeof(line));
	ngui_text_ellipsis(ui, status, status.x + 6,
			   status.y + (status.h - 16) / 2, line,
			   ui->theme.text_dim);

	if (body.w <= 0 || body.h <= 0)
		return;

	ngui_fill_rect(ui, body, VIEW_VIEW_BG);

	if (!v.have_img) {
		if (v.error)
			draw_message(ui, body, v.err);
		else {
			char msg[48];

			s_copy(msg, "Loading ", sizeof(msg));
			{
				unsigned int n = s_len(msg);

				s_append(msg, &n, v.name, sizeof(msg));
				s_append(msg, &n, "...", sizeof(msg));
			}
			draw_message(ui, body, msg);
		}
		return;
	}

	{
		unsigned int scale = view_scale(ui);
		unsigned int sw =
			(unsigned int)(((unsigned long long)v.iw * scale) >> 16);
		unsigned int sh =
			(unsigned int)(((unsigned long long)v.ih * scale) >> 16);
		struct img_blit blit;

		if (!sw) sw = 1;
		if (!sh) sh = 1;
		clamp_view(ui, scale);
		blit.dst = ui->px;
		blit.dst_w = ui->w;
		blit.dst_h = ui->h;
		blit.clip_x = body.x;
		blit.clip_y = body.y;
		blit.clip_w = body.w;
		blit.clip_h = body.h;
		blit.x = body.x + v.off_x;
		blit.y = body.y + v.off_y;
		blit.scale = scale;
		blit.bg = VIEW_CHK1;
		blit.bg2 = VIEW_CHK2;
		blit.flags = IMG_BLIT_CHECKER;
		if (v.dragging)
			blit.flags |= IMG_BLIT_FAST;
		if (v.img.alpha)
			img_draw(&v.img, &blit);
		else {
			blit.flags &= ~IMG_BLIT_CHECKER;
			img_draw(&v.img, &blit);
		}
		(void)sw;
		(void)sh;
	}
}

static int create_window(unsigned int base_w, unsigned int base_h,
			 const char *title, struct win_info *wi)
{
	unsigned int w = base_w, h = base_h;

	if (w < 320) w = 320;
	if (h < 200) h = 200;
	if (w > 960) w = 960;
	if (h > 640) h = 640;
	for (;;) {
		if (u_win_create(w, h + VIEW_BAR_H, title, wi) == 0)
			return 0;
		if (w <= 240 && h <= 160)
			break;
		w = w * 4 / 5;
		h = h * 4 / 5;
		if (w < 240) w = 240;
		if (h < 160) h = 160;
	}
	return -1;
}

static void error_exit(const char *msg)
{
	struct win_info wi;
	struct ngui ui;
	int quit = 0;

	if (u_win_create(480, 240 + VIEW_BAR_H, "view", &wi) != 0) {
		u_puts("view: ");
		u_puts(msg);
		u_puts("\n");
		u_exit(1);
	}
	ngui_init(&ui, (unsigned short *)wi.fb, (int)wi.width, (int)wi.height);
	ngui_font_attach(&ui);
	v.error = 1;
	s_copy(v.err, msg, sizeof(v.err));
	v.have_img = 0;
	for (;;) {
		if (!ngui_begin(&ui)) {
			u_sleep_ms(10);
			continue;
		}
		{
			int k = ngui_key(&ui);

			if (k == NGUI_KEY_ESC || k == 'q' || k == 0x03)
				quit = 1;
		}
		draw(&ui);
		ngui_end(&ui);
		if (quit)
			break;
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(1);
}

void _start(const char *arg)
{
	struct img_info info;
	struct win_info wi;
	struct img_options opt;
	struct sys_stat st;
	unsigned int n, fcap, est;
	int r;

	if (!arg || !arg[0]) {
		u_puts("usage: view <file>\n");
		u_exit(1);
	}
	base_name(arg, v.name, sizeof(v.name));
	s_copy(v.title, "view ", sizeof(v.title));
	{
		unsigned int tn = s_len(v.title);

		s_append(v.title, &tn, v.name, sizeof(v.title));
	}

	fcap = VIEW_FILE_CAP;
	if (u_stat(arg, &st) == 0 && !st.is_dir && st.size > 0 &&
	    st.size < VIEW_FILE_CAP)
		fcap = st.size + 1u;
	v.file = (unsigned char *)malloc(fcap);
	if (!v.file)
		error_exit("Not enough memory for the image");

	n = (unsigned int)img_read_file(arg, v.file, fcap);
	if ((int)n < 0) {
		if ((int)n == IMG_ERR_TOOBIG)
			error_exit("Image file is larger than 4 MB");
		error_exit(img_error_string((int)n));
	}
	v.file_size = n;

	r = img_sniff(v.file, n, &info);
	if (r != 0)
		error_exit(img_error_string(r));

	est = img_estimate(&info);
	if (est == 0xFFFFFFFFu || est > VIEW_HEAP_MAX ||
	    (unsigned long long)n + est > VIEW_HEAP_MAX)
		error_exit("Image needs more than 32 MB");
	v.arena = (unsigned char *)malloc(est);
	if (!v.arena)
		error_exit("Not enough memory for the image");
	img_mem_init(&v.mem, v.arena, est);
	v.iw = info.width;
	v.ih = info.height;
	v.format = info.format;

	if (create_window(info.width, info.height, v.title, &wi) != 0) {
		u_puts("view: no graphics window\n");
		u_exit(1);
	}
	ngui_init(&v.ui, (unsigned short *)wi.fb, (int)wi.width, (int)wi.height);
	ngui_font_attach(&v.ui);
	v.fit = 1;
	v.zoom = 65536;
	v.loading = 1;
	v.dirty = 1;
	v.last_w = (int)wi.width;
	v.last_h = (int)wi.height;
	{
		struct ngui *ui = &v.ui;

		/* Draw the loading frame once; the input loop starts after decode. */
		ngui_begin(ui);
		draw(ui);
		ngui_end(ui);
	}

	opt.max_pixels = 0;
	r = img_decode(v.file, n, &opt, &v.mem, &v.img);
	v.loading = 0;
	if (r != 0) {
		s_copy(v.err, img_error_string(r), sizeof(v.err));
		v.error = 1;
		v.have_img = 0;
	} else {
		v.have_img = 1;
		v.error = 0;
		v.iw = v.img.w;
		v.ih = v.img.h;
		v.format = v.img.format;
		v.fit = 1;
	}
	if (v.have_img && v.format == IMG_FMT_GIF) {
		unsigned int frames = 0, delay = 100;
		int ha = 0;

		if (img_gif_frames(v.file, v.file_size, &frames, &delay, &ha) == 0 &&
		    frames > 1) {
			v.gif_frames = frames;
			v.gif_index = 0;
			v.gif_delay = delay;
			v.gif_next_ms = u_ticks_ms() + delay;
		}
	}
	v.dirty = 1;

	for (;;) {
		struct ngui *ui = &v.ui;
		struct ngui_rect body = view_body(ui);
		int close = 0;
		int k;
		unsigned int now = u_ticks_ms();

		if (v.gif_frames > 1 &&
		    (int)(now - v.gif_next_ms) >= 0) {
			unsigned int d = v.gif_delay;

			v.gif_index = (v.gif_index + 1) % v.gif_frames;
			if (img_decode_gif_frame(v.file, v.file_size,
						 v.gif_index, &v.mem, &v.img,
						 &d) == 0) {
				if (d < 20) d = 20;
				v.gif_delay = d;
				v.dirty = 1;
			} else {
				v.gif_frames = 0;
			}
			v.gif_next_ms = u_ticks_ms() + v.gif_delay;
		}

		ngui_begin(ui);
		if (ui->w != v.last_w || ui->h != v.last_h) {
			/* size changed; redraw at the new dimensions */
			v.last_w = ui->w;
			v.last_h = ui->h;
			v.dirty = 1;
		}

		while ((k = ngui_key(ui)) != 0) {
			if (k == NGUI_KEY_ESC || k == 'q' || k == 0x03) {
				close = 1;
				break;
			} else if (k == 'f' || k == '0') {
				view_fit();
				v.dirty = 1;
			} else if (k == '1') {
				view_one(ui);
				v.dirty = 1;
			} else if (k == '+' || k == '=') {
				view_zoom_step(ui, 1);
				v.dirty = 1;
			} else if (k == '-' || k == '_') {
				view_zoom_step(ui, 0);
				v.dirty = 1;
			} else if (k == NGUI_KEY_LEFT || k == NGUI_KEY_RIGHT ||
				   k == NGUI_KEY_UP || k == NGUI_KEY_DOWN) {
				if (v.have_img) {
					if (v.fit)
						view_set_scale(ui, view_scale(ui));
					if (k == NGUI_KEY_LEFT)
						v.off_x += 32;
					if (k == NGUI_KEY_RIGHT)
						v.off_x -= 32;
					if (k == NGUI_KEY_UP)
						v.off_y += 32;
					if (k == NGUI_KEY_DOWN)
						v.off_y -= 32;
					clamp_view(ui, view_scale(ui));
					v.dirty = 1;
				}
			}
		}
		if (close)
			break;

		if (ui->pressed && ui->press_x >= body.x && ui->press_y >= body.y &&
		    ui->press_x < body.x + body.w && ui->press_y < body.y + body.h &&
		    v.have_img) {
			v.dragging = 1;
			v.drag_x = ui->mouse_x;
			v.drag_y = ui->mouse_y;
			v.dirty = 1;
		}
		if (v.dragging) {
			if (!(ui->buttons & 1) || ui->released) {
				v.dragging = 0;
				v.dirty = 1;
			} else {
				int dx = ui->mouse_x - v.drag_x;
				int dy = ui->mouse_y - v.drag_y;

				if (dx || dy) {
					v.off_x += dx;
					v.off_y += dy;
					v.drag_x = ui->mouse_x;
					v.drag_y = ui->mouse_y;
					clamp_view(ui, view_scale(ui));
					v.dirty = 1;
				}
			}
		}
		if (ui->pressed || ui->released)
			v.dirty = 1;

		if (v.dirty) {
			v.dirty = 0;
			draw(ui);
			ngui_end_ex(ui, 1);
		} else {
			ngui_end_ex(ui, 0);
		}
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(0);
}
