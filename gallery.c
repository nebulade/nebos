/* gallery: an interactive showcase of every widget and primitive in ngui.
 *
 * One scrolling page holds a section per part of the toolkit: typography,
 * the drawing primitives, the theme palette, every procedural icon, buttons,
 * text fields, the scroll pane and the ngui_cut_* layout helpers.  The
 * controls are live, so all the input paths the toolkit offers are exercised.
 * ESC closes the window after a focused field has cancelled its edit. */

#include "ucmd_lib.h"
#include "ngui.h"

#define START_W         860
#define START_H         600

#define HEADER_H        64
#define NAV_H           35
#define STATUS_H        24

#define SEC_HEAD_H      34
#define SEC_GAP         24
#define PAGE_BOTTOM     24
#define PAGE_PAD        12
#define CARD_PAD        16

#define TYPO_H          220
#define SHAPES_H        154
#define THEME_H         140
#define ICONS_H         100
#define BUTTONS_H       140
#define INPUTS_H        250
#define SCROLL_H        220
#define LAYOUT_H        190

#define LIST_ROWS       12
#define LIST_ROW_H      24

enum section {
	SEC_TEXT,
	SEC_SHAPES,
	SEC_THEME,
	SEC_ICONS,
	SEC_BUTTONS,
	SEC_INPUTS,
	SEC_SCROLL,
	SEC_LAYOUT,
	SEC_COUNT
};

enum {
	ID_HEADER_HOME = 1,
	ID_HEADER_PAGE_UP,
	ID_HEADER_RESET,

	ID_BTN_CLICK,
	ID_BTN_RESET,
	ID_ICON_PLUS,
	ID_ICON_REFRESH,
	ID_ICON_PREV,
	ID_ICON_NEXT,

	ID_IN_NAME,
	ID_IN_EMAIL,
	ID_IN_EVENTS,
	ID_IN_DISABLED,
	ID_BTN_RESET_FIELDS,

	ID_NAV_BASE = 0x100,
	ID_ICON_BASE = 0x200
};

static const int sec_card_h[SEC_COUNT] = {
	TYPO_H, SHAPES_H, THEME_H, ICONS_H,
	BUTTONS_H, INPUTS_H, SCROLL_H, LAYOUT_H
};

static const char *const sec_titles[SEC_COUNT] = {
	"Text", "Shapes", "Theme", "Icons",
	"Buttons", "Inputs", "Scroll", "Layout"
};

static const char *const sec_descs[SEC_COUNT] = {
	"styles, truncation and measurement",
	"rectangles, frames, lines and clipping",
	"the ngui_theme_dark palette",
	"all nine procedural icons",
	"buttons and icon buttons",
	"single-line text fields",
	"a nested draggable scroll pane",
	"ngui_cut_* layout helpers"
};

static const char *const color_names[15] = {
	"bg", "panel", "panel_alt", "border", "text",
	"text_dim", "accent", "selection", "button", "button_hot",
	"button_down", "icon_folder", "icon_file", "icon_disk", "error"
};

static const enum ngui_icon icon_list[9] = {
	NGUI_ICON_FOLDER, NGUI_ICON_FILE, NGUI_ICON_DISK,
	NGUI_ICON_UP, NGUI_ICON_HOME, NGUI_ICON_REFRESH,
	NGUI_ICON_PLUS, NGUI_ICON_CHEVRON_LEFT, NGUI_ICON_CHEVRON_RIGHT
};

static const char *const icon_names[9] = {
	"folder", "file", "disk", "up", "home",
	"refresh", "plus", "left", "right"
};

static const char *const list_items[LIST_ROWS] = {
	"ngui_fill_rect", "ngui_frame", "ngui_line", "ngui_text",
	"ngui_text_styled", "ngui_icon", "ngui_button",
	"ngui_icon_button", "ngui_text_input", "ngui_scroll_begin",
	"ngui_scroll_end", "ngui_cut_left"
};

static struct ngui ui;
static struct ngui_scroll page;
static struct ngui_scroll list_scroll;
static struct ngui_input name_in;
static struct ngui_input email_in;
static struct ngui_input events_in;
static struct ngui_input disabled_in;

static int sec_y[SEC_COUNT];
static int sec_content_h;

static int button_clicks;
static int icon_clicks;
static int list_selected = 2;
static int feedback_error;
static char event_text[96];
static char input_feedback[160];
static int quit;

/* ---- tiny freestanding string helpers ---- */

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

static void s_append(char *d, unsigned int *n, const char *s,
		     unsigned int cap)
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

static void event_set(const char *s)
{
	s_copy(event_text, s, sizeof(event_text));
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

/* ---- layout ---- */

static void gallery_layout(void)
{
	int i, y = 0;

	for (i = 0; i < SEC_COUNT; i++) {
		sec_y[i] = y;
		y += SEC_HEAD_H + sec_card_h[i] + SEC_GAP;
	}
	y -= SEC_GAP;
	sec_content_h = y + PAGE_BOTTOM;
}

static int page_percent(void)
{
	int max = page.content_h - page.view_h;

	if (max <= 0)
		return 0;
	return page.offset * 100 / max;
}

static int active_section(void)
{
	int max = page.content_h - page.view_h;
	int probe = page.offset + 60;
	int i, active = 0;

	if (max > 0 && page.offset >= max - 1)
		return SEC_LAYOUT;
	for (i = 0; i < SEC_COUNT; i++)
		if (sec_y[i] <= probe)
			active = i;
	return active;
}

static const char *focus_name(void)
{
	switch (ngui_focus(&ui)) {
	case ID_IN_NAME:
		return "name";
	case ID_IN_EMAIL:
		return "prefilled";
	case ID_IN_EVENTS:
		return "events";
	default:
		return "none";
	}
}

/* Every widget is clipped by the scroll pane, but ngui's hit tests use the
 * widget rectangle directly.  Skip widgets that are completely outside the
 * current clip so a click on the status bar can never reach a scrolled-away
 * control. */
static int rect_visible(struct ngui *g, struct ngui_rect r)
{
	struct ngui_rect c;

	if (g->nclip > 0)
		c = g->clip[g->nclip - 1];
	else
		c = rect_make(0, 0, g->w, g->h);
	return r.x < c.x + c.w && r.x + r.w > c.x &&
	       r.y < c.y + c.h && r.y + r.h > c.y;
}

static int gallery_button(struct ngui *g, unsigned int id,
			  struct ngui_rect r, const char *label)
{
	if (!rect_visible(g, r))
		return 0;
	return ngui_button(g, id, r, label);
}

static int gallery_icon_button(struct ngui *g, unsigned int id,
			       struct ngui_rect r, enum ngui_icon icon)
{
	if (!rect_visible(g, r))
		return 0;
	return ngui_icon_button(g, id, r, icon);
}

static int gallery_input(struct ngui *g, unsigned int id,
			 struct ngui_rect r, struct ngui_input *in)
{
	if (!rect_visible(g, r))
		return 0;
	return ngui_text_input(g, id, r, in);
}

static void center_text(struct ngui *g, struct ngui_rect r, const char *s,
			unsigned short color)
{
	int tw = (int)ngui_text_width_ui(g, s);
	int x = r.x + (r.w - tw) / 2;
	int y = r.y + (r.h - 16) / 2;

	ngui_push_clip(g, r);
	ngui_text(g, x, y, s, color);
	ngui_pop_clip(g);
}

/* ---- state ---- */

static void reset_demo(struct ngui *g)
{
	button_clicks = 0;
	icon_clicks = 0;
	list_selected = 2;
	ngui_input_set(&name_in, "");
	ngui_input_set(&email_in, "ada@nebos.org");
	ngui_input_set(&events_in, "");
	ngui_input_set(&disabled_in, "Read-only value");
	ngui_set_focus(g, 0);
	s_copy(input_feedback, "No events yet - edit a field.",
		sizeof(input_feedback));
	feedback_error = 0;
	event_set("widgets reset");
	list_scroll.offset = 0;
	ngui_scroll_to(&page, 0);
	ngui_invalidate(g);
}

static void note_input(struct ngui *g, int result, const char *field,
		       const struct ngui_input *in)
{
	char b[160];
	unsigned int n = 0;

	(void)g;
	if (!result)
		return;
	if (result & NGUI_INPUT_SUBMIT) {
		s_append(b, &n, "SUBMIT ", sizeof(b));
		s_append(b, &n, field, sizeof(b));
		s_append(b, &n, ": ", sizeof(b));
		s_append(b, &n, in->text, sizeof(b));
		feedback_error = 0;
	} else if (result & NGUI_INPUT_CANCEL) {
		s_append(b, &n, "CANCEL ", sizeof(b));
		s_append(b, &n, field, sizeof(b));
		s_append(b, &n, " - focus released", sizeof(b));
		feedback_error = 1;
	} else if (result & NGUI_INPUT_CHANGED) {
		s_append(b, &n, "CHANGED ", sizeof(b));
		s_append(b, &n, field, sizeof(b));
		s_append(b, &n, ": ", sizeof(b));
		s_append(b, &n, in->text, sizeof(b));
		feedback_error = 0;
	} else {
		return;
	}
	s_copy(input_feedback, b, sizeof(input_feedback));
	s_copy(event_text, b, sizeof(event_text));
}

/* ---- header and navigation ---- */

static void draw_header(struct ngui *g, struct ngui_rect r)
{
	int bx = r.x + r.w - 16 - (3 * 30 + 2 * 8);
	int by = r.y + 18;
	int sub_right = bx > r.x + 560 ? bx - 14 : r.x + r.w - 18;
	int step;

	ngui_fill_rect(g, r, g->theme.panel);
	ngui_fill_rect(g, rect_make(r.x, r.y, r.w, 2), g->theme.accent);

	ngui_text_styled(g, r.x + 18, r.y + 9, "ngui", 4,
			 g->theme.accent, 2, 1);
	ngui_text_styled(g, r.x + 18 + 64 + 10, r.y + 9,
			 "widget gallery", 14, g->theme.text, 2, 0);
	ngui_text_ellipsis(g, rect_make(r.x + 18, r.y + 40,
					sub_right - (r.x + 18), 16),
			   r.x + 18, r.y + 40,
			   "Every control and primitive exported by ngui.h - resize and play",
			   g->theme.text_dim);
	ngui_fill_rect(g, rect_make(r.x, r.y + r.h - 1, r.w, 1),
		       g->theme.border);

	if (bx < r.x + 560)
		return;

	ngui_fill_rect(g, rect_make(bx - 14, r.y + 18, 1, 28),
		       g->theme.panel_alt);

	if (gallery_icon_button(g, ID_HEADER_HOME,
				rect_make(bx, by, 30, 28),
				NGUI_ICON_HOME)) {
		ngui_scroll_to(&page, 0);
		event_set("jumped to top");
		ngui_invalidate(g);
	}
	if (gallery_icon_button(g, ID_HEADER_PAGE_UP,
				rect_make(bx + 38, by, 30, 28),
				NGUI_ICON_UP)) {
		step = page.view_h > 120 ? page.view_h * 2 / 3 : 120;
		ngui_scroll_to(&page, page.offset - step);
		event_set("scrolled up");
		ngui_invalidate(g);
	}
	if (gallery_icon_button(g, ID_HEADER_RESET,
				rect_make(bx + 76, by, 30, 28),
				NGUI_ICON_REFRESH)) {
		reset_demo(g);
	}
}

static void draw_nav(struct ngui *g, struct ngui_rect r)
{
	int x = r.x + 16;
	int y = r.y + 5;
	int active = active_section();
	int i;

	ngui_fill_rect(g, r, g->theme.bg);
	ngui_fill_rect(g, rect_make(r.x, r.y + r.h - 1, r.w, 1),
		       g->theme.border);

	for (i = 0; i < SEC_COUNT; i++) {
		int w = (int)ngui_text_width_ui(g, sec_titles[i]) + 20;

		if (x + w > r.x + r.w - 12)
			break;
		if (gallery_button(g, ID_NAV_BASE + (unsigned int)i,
				   rect_make(x, y, w, 25), sec_titles[i])) {
			char b[96];
			unsigned int n = 0;

			ngui_scroll_to(&page, sec_y[i]);
			s_append(b, &n, "jumped to ", sizeof(b));
			s_append(b, &n, sec_titles[i], sizeof(b));
			event_set(b);
			ngui_invalidate(g);
		}
		if (i == active)
			ngui_fill_rect(g, rect_make(x + 4, y + 25, w - 8, 2),
				       g->theme.accent);
		x += w + 6;
	}
}

/* ---- sections ---- */

static void draw_section_head(struct ngui *g, int x, int y, int w, int idx)
{
	char num[3];
	int dx;

	num[0] = '0';
	num[1] = (char)('1' + idx);
	num[2] = '\0';

	ngui_fill_rect(g, rect_make(x, y + 5, 3, 18), g->theme.accent);
	ngui_text(g, x + 12, y + 6, num, g->theme.accent);
	ngui_text_styled(g, x + 34, y + 6, sec_titles[idx],
			 s_len(sec_titles[idx]), g->theme.text, 1, 1);

	dx = x + 34 + (int)ngui_text_width_ui(g, sec_titles[idx]) + 14;
	if (dx < x + w - 16)
		ngui_text_ellipsis(g, rect_make(dx, y + 6, x + w - dx, 16),
				   dx, y + 6, sec_descs[idx],
				   g->theme.text_dim);
}

static void draw_typography(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int y = card.y + 14;
	int label_w = 190;
	int value_x = x + label_w;
	int value_w = card.x + card.w - CARD_PAD - value_x;
	char buf[64];
	unsigned int n = 0;

	ngui_text(g, x, y + 4, "Heading", g->theme.text_dim);
	ngui_push_clip(g, rect_make(value_x, y, value_w, 32));
	ngui_text_styled(g, value_x, y, "Widget gallery", 14,
			 g->theme.text, 2, 1);
	ngui_pop_clip(g);
	y += 38;

	ngui_text(g, x, y + 2, "Body", g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2,
			   "The quick brown fox jumps over the lazy dog.",
			   g->theme.text);
	y += 22;

	ngui_text(g, x, y + 2, "Dim / secondary", g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2,
			   "Secondary text recedes into the background.",
			   g->theme.text_dim);
	y += 22;

	ngui_text(g, x, y + 2, "Accent", g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2,
			   "Accent: selection, focus and interactive detail.",
			   g->theme.accent);
	y += 22;

	ngui_text(g, x, y + 2, "Error", g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2,
			   "Error: the widget could not be opened.",
			   g->theme.error);
	y += 22;

	ngui_text(g, x, y + 2, "ngui_text_n (n = 17)", g->theme.text_dim);
	ngui_push_clip(g, rect_make(value_x, y + 2, value_w, 16));
	ngui_text_n(g, value_x, y + 2, "draws only seventeen chars", 17,
		    g->theme.text);
	ngui_pop_clip(g);
	y += 22;

	ngui_text(g, x, y + 2, "ngui_text_ellipsis", g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2,
			   "This caption is deliberately much too long to fit inside its rectangle, so ngui crops it with three dots.",
			   g->theme.text);
	y += 22;

	ngui_text(g, x, y + 2, "ngui_text_width", g->theme.text_dim);
	n = 0;
	s_append(buf, &n, "ngui_text_width(\"ngui\") = ", sizeof(buf));
	s_append_uint(buf, &n, ngui_text_width_ui(g, "ngui"), sizeof(buf));
	s_append(buf, &n, " px", sizeof(buf));
	ngui_text_ellipsis(g, rect_make(value_x, y + 2, value_w, 16),
			   value_x, y + 2, buf, g->theme.text);
}

static void draw_shapes(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int top = card.y + 16;
	int label_w = 120;
	int right_x = card.x + card.w / 2 + 8;
	int right_w = card.x + card.w - CARD_PAD - right_x;
	struct ngui_rect fr, inner;
	int px[7] = {0, 22, 44, 66, 88, 110, 132};
	int py[7] = {32, 18, 26, 8, 16, 4, 12};
	int i;

	ngui_text(g, x, top + 3, "fill_rect", g->theme.text_dim);
	for (i = 0; i < 5; i++) {
		unsigned short colors[5];
		struct ngui_rect sw;

		colors[0] = g->theme.accent;
		colors[1] = g->theme.selection;
		colors[2] = g->theme.icon_folder;
		colors[3] = g->theme.error;
		colors[4] = g->theme.panel_alt;
		sw = rect_make(x + label_w + i * 28, top, 22, 22);
		ngui_fill_rect(g, sw, colors[i]);
		ngui_frame(g, sw, g->theme.border);
	}

	ngui_text(g, x, top + 37, "frame / inset", g->theme.text_dim);
	fr = rect_make(x + label_w, top + 34, 150, 28);
	ngui_fill_rect(g, fr, g->theme.panel_alt);
	ngui_frame(g, fr, g->theme.border);
	inner = fr;
	ngui_inset(&inner, 5);
	ngui_frame(g, inner, g->theme.accent);

	ngui_text(g, x, top + 75, "ngui_line", g->theme.text_dim);
	for (i = 0; i < 6; i++)
		ngui_line(g, x + label_w + px[i], top + 72 + py[i],
			  x + label_w + px[i + 1], top + 72 + py[i + 1],
			  g->theme.accent);

	if (right_w < 160)
		return;

	ngui_text(g, right_x, card.y + 16, "push_clip / pop_clip",
		  g->theme.text_dim);
	{
		struct ngui_rect vp = rect_make(right_x, card.y + 36,
						right_w, card.h - 52);

		ngui_fill_rect(g, vp, g->theme.bg);
		ngui_push_clip(g, vp);
		for (i = -vp.h; i < vp.w; i += 16) {
			unsigned short c = ((i / 16) & 1) ?
				g->theme.panel_alt : g->theme.button;

			ngui_line(g, vp.x + i, vp.y,
				  vp.x + i + vp.h, vp.y + vp.h, c);
		}
		ngui_fill_rect(g, rect_make(vp.x - 20, vp.y + 14, 60, 30),
			       g->theme.icon_folder);
		ngui_text_styled(g, vp.x - 24, vp.y + vp.h / 2 - 24,
				 "CLIPPED", 7, g->theme.accent, 3, 1);
		ngui_pop_clip(g);
		ngui_frame(g, vp, g->theme.accent);
	}
}

static void draw_theme(struct ngui *g, struct ngui_rect card)
{
	unsigned short colors[15];
	int colw = (card.w - 2 * CARD_PAD) / 3;
	int i;

	colors[0] = g->theme.bg;
	colors[1] = g->theme.panel;
	colors[2] = g->theme.panel_alt;
	colors[3] = g->theme.border;
	colors[4] = g->theme.text;
	colors[5] = g->theme.text_dim;
	colors[6] = g->theme.accent;
	colors[7] = g->theme.selection;
	colors[8] = g->theme.button;
	colors[9] = g->theme.button_hot;
	colors[10] = g->theme.button_down;
	colors[11] = g->theme.icon_folder;
	colors[12] = g->theme.icon_file;
	colors[13] = g->theme.icon_disk;
	colors[14] = g->theme.error;

	for (i = 0; i < 15; i++) {
		int cx = card.x + CARD_PAD + (i % 3) * colw;
		int cy = card.y + 14 + (i / 3) * 22;
		struct ngui_rect sw = rect_make(cx, cy, 16, 16);

		ngui_fill_rect(g, sw, colors[i]);
		ngui_frame(g, sw, g->theme.border);
		if (colw > 20)
			ngui_text_ellipsis(g, rect_make(cx + 24, cy,
							colw - 24, 16),
					   cx + 24, cy, color_names[i],
					   g->theme.text);
	}
}

static void draw_icons(struct ngui *g, struct ngui_rect card)
{
	int cell = (card.w - 2 * CARD_PAD) / 9;
	int bs = cell >= 56 ? 40 : 30;
	int y = card.y + 12;
	int i;
	char count[24];
	unsigned int n = 0;

	if (cell < 20)
		return;
	for (i = 0; i < 9; i++) {
		int cx = card.x + CARD_PAD + i * cell;
		int bx = cx + (cell - bs) / 2;
		int tw, tx, ly = y + bs + 4;
		struct ngui_rect lr;

		if (gallery_icon_button(g, ID_ICON_BASE + (unsigned int)i,
					rect_make(bx, y, bs, bs),
					icon_list[i])) {
			char b[64];
			unsigned int m = 0;

			icon_clicks++;
			s_append(b, &m, "icon '", sizeof(b));
			s_append(b, &m, icon_names[i], sizeof(b));
			s_append(b, &m, "' - ", sizeof(b));
			s_append_uint(b, &m, (unsigned int)icon_clicks,
				      sizeof(b));
			s_append(b, &m, icon_clicks == 1 ?
				  " click" : " clicks", sizeof(b));
			event_set(b);
			ngui_invalidate(g);
		}

		tw = (int)ngui_text_width_ui(g, icon_names[i]);
		tx = cx + (cell - tw) / 2;
		lr = rect_make(cx + 2, ly, cell - 4, 16);
		if (tx < lr.x)
			tx = lr.x;
		ngui_text_ellipsis(g, lr, tx, ly, icon_names[i],
				   g->theme.text_dim);
	}

	s_append(count, &n, "icon clicks: ", sizeof(count));
	s_append_uint(count, &n, (unsigned int)icon_clicks, sizeof(count));
	ngui_text_ellipsis(g, rect_make(card.x + CARD_PAD, card.y + 74,
					card.w - 2 * CARD_PAD, 16),
			   card.x + CARD_PAD, card.y + 74,
			   "Click an icon button - every one is live",
			   g->theme.text_dim);
	if (card.w > 650) {
		int cw = (int)ngui_text_width_ui(g, count);

		ngui_text(g, card.x + card.w - CARD_PAD - cw,
			  card.y + 74, count, g->theme.text_dim);
	}
}

static void draw_buttons(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int y = card.y + 18;
	char num[16];
	char buf[32];
	unsigned int n;

	if (gallery_button(g, ID_BTN_CLICK, rect_make(x, y, 96, 28),
			   "Click me")) {
		char b[64];
		unsigned int m = 0;

		button_clicks++;
		s_append(b, &m, "button clicked - total ", sizeof(b));
		s_append_uint(b, &m, (unsigned int)button_clicks, sizeof(b));
		event_set(b);
	}
	if (gallery_button(g, ID_BTN_RESET, rect_make(x + 104, y, 80, 28),
			   "Reset")) {
		button_clicks = 0;
		event_set("button counter reset");
	}
	if (gallery_icon_button(g, ID_ICON_PLUS,
				rect_make(x + 192, y, 30, 28),
				NGUI_ICON_PLUS)) {
		char b[64];
		unsigned int m = 0;

		button_clicks++;
		s_append(b, &m, "icon button '+' - total ", sizeof(b));
		s_append_uint(b, &m, (unsigned int)button_clicks, sizeof(b));
		event_set(b);
	}
	if (gallery_icon_button(g, ID_ICON_REFRESH,
				rect_make(x + 228, y, 30, 28),
				NGUI_ICON_REFRESH)) {
		button_clicks = 0;
		event_set("button counter reset");
	}
	if (gallery_icon_button(g, ID_ICON_PREV,
				rect_make(x + 264, y, 30, 28),
				NGUI_ICON_CHEVRON_LEFT))
		event_set("chevron left pressed");
	if (gallery_icon_button(g, ID_ICON_NEXT,
				rect_make(x + 300, y, 30, 28),
				NGUI_ICON_CHEVRON_RIGHT))
		event_set("chevron right pressed");

	n = 0;
	s_append_uint(num, &n, (unsigned int)button_clicks, sizeof(num));
	if (card.w > 620) {
		int right = card.x + card.w - CARD_PAD;
		int vw = (int)ngui_text_width_px(g, num, n, g->font_px * 2, 1, 0, 0);

		ngui_text(g, right - 96, card.y + 18, "TOTAL CLICKS",
			  g->theme.text_dim);
		ngui_text_styled(g, right - vw, card.y + 40, num, n,
				 g->theme.accent, 2, 1);
	} else {
		unsigned int m = 0;

		s_append(buf, &m, "clicks: ", sizeof(buf));
		s_append_uint(buf, &m, (unsigned int)button_clicks,
			      sizeof(buf));
		ngui_text(g, x, card.y + 56, buf, g->theme.accent);
	}

	ngui_text_ellipsis(g, rect_make(x, card.y + 72, card.w - 2 * CARD_PAD,
					16),
			   x, card.y + 72,
			   "Buttons fire on release inside; hover paints an accent frame.",
			   g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(x, card.y + 94, card.w - 2 * CARD_PAD,
					16),
			   x, card.y + 94,
			   "Pressed buttons keep the accent fill until you let go.",
			   g->theme.text_dim);
	ngui_text_ellipsis(g, rect_make(x, card.y + 116, card.w - 2 * CARD_PAD,
					16),
			   x, card.y + 116,
			   "ngui_button(ui, id, rect, label) / ngui_icon_button(ui, id, rect, icon)",
			   g->theme.border);
}

static void draw_inputs(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int top = card.y + 16;
	int label_w = 126;
	int iw = card.w / 2 - 80;
	int field_y = top;
	int help_x, help_w, help_h;
	int result;
	int feedback_y = top + 144;

	if (iw < 220)
		iw = 220;
	if (iw > 340)
		iw = 340;

	ngui_text(g, x, field_y + 4, "Name", g->theme.text_dim);
	result = gallery_input(g, ID_IN_NAME,
			       rect_make(x + label_w, field_y, iw, 24),
			       &name_in);
	note_input(g, result, "name", &name_in);
	field_y += 36;

	ngui_text(g, x, field_y + 4, "Prefilled", g->theme.text_dim);
	result = gallery_input(g, ID_IN_EMAIL,
			       rect_make(x + label_w, field_y, iw, 24),
			       &email_in);
	note_input(g, result, "prefilled", &email_in);
	field_y += 36;

	ngui_text(g, x, field_y + 4, "Events", g->theme.text_dim);
	result = gallery_input(g, ID_IN_EVENTS,
			       rect_make(x + label_w, field_y, iw, 24),
			       &events_in);
	note_input(g, result, "events", &events_in);
	field_y += 36;

	ngui_text(g, x, field_y + 4, "Disabled", g->theme.text_dim);
	/* A disabled field ignores clicks and keys, so it never reports an
	 * event; the value stays in the same proportional face, dimmed. */
	gallery_input(g, ID_IN_DISABLED,
		      rect_make(x + label_w, field_y, iw, 24),
		      &disabled_in);

	help_x = x + label_w + iw + 24;
	help_w = card.x + card.w - CARD_PAD - help_x;
	help_h = 200;

	if (help_w >= 150) {
		int hx = help_x;
		int hy = top;

		ngui_fill_rect(g, rect_make(hx, hy, help_w, help_h),
			       g->theme.bg);
		ngui_frame(g, rect_make(hx, hy, help_w, help_h),
			   g->theme.border);
		ngui_text(g, hx + 10, hy + 9, "TEXT INPUT",
			  g->theme.accent);
		ngui_text(g, hx + 10, hy + 36, "Return - submit",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 54, "Esc - cancel focus",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 72, "Arrows - move caret",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 90, "Ctrl+Left/Right - words",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 108, "Ctrl+Del/Bksp - words",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 126, "Drag - select, Ctrl+A all",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 144, "Home/End, Ctrl+U - ends/clear",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 162, "Tab - focus first",
			  g->theme.text_dim);
		ngui_text(g, hx + 10, hy + 180, "Disabled - no input",
			  g->theme.text_dim);
	}

	if (input_feedback[0])
		ngui_text_ellipsis(g, rect_make(x, feedback_y,
						(help_w >= 150 ?
						 help_x - x - 12 :
						 card.w - 2 * CARD_PAD),
						16),
				   x, feedback_y, input_feedback,
				   feedback_error ? g->theme.error :
						    g->theme.accent);
	ngui_text_ellipsis(g, rect_make(x, top + 166,
					(help_w >= 150 ?
					 help_x - x - 12 :
					 card.w - 2 * CARD_PAD), 16),
			   x, top + 166,
			   "Return submits, Esc cancels, Ctrl+U clears.",
			   g->theme.text_dim);

	if (gallery_button(g, ID_BTN_RESET_FIELDS,
			   rect_make(x, top + 186, 104, 26),
			   "Reset fields")) {
		ngui_input_set(&name_in, "");
		ngui_input_set(&email_in, "ada@nebos.org");
		ngui_input_set(&events_in, "");
		ngui_input_set(&disabled_in, "Read-only value");
		ngui_set_focus(g, 0);
		s_copy(input_feedback, "Fields reset to their defaults.",
			sizeof(input_feedback));
		feedback_error = 0;
		event_set("input fields reset");
	}
	if (help_w >= 150)
		ngui_text(g, x + 120, top + 192,
			  "ngui_input_set() rewrites a field.",
			  g->theme.border);
}

static void draw_scroll(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int top = card.y + 16;
	int pane_w = card.w - 2 * CARD_PAD - 190;
	int pane_h = card.h - 56;
	struct ngui_rect pane;
	int help_x, help_w;
	int i;

	if (pane_w > 520)
		pane_w = 520;
	if (pane_w < 260)
		pane_w = 260;
	if (card.w < 590)
		pane_w = card.w - 2 * CARD_PAD;
	if (pane_w < 120)
		return;

	pane = rect_make(x, card.y + 40, pane_w, pane_h);
	ngui_text_ellipsis(g, rect_make(x, top, card.w - 2 * CARD_PAD, 16),
			   x, top,
			   "A nested pane: click a row, drag its scrollbar.",
			   g->theme.text_dim);

	list_scroll.content_h = LIST_ROWS * LIST_ROW_H;
	ngui_fill_rect(g, pane, g->theme.bg);
	ngui_scroll_begin(g, pane, &list_scroll);

	if (g->pressed && ngui_rect_contains(&pane, g->press_x, g->press_y) &&
	    g->press_x < pane.x + pane.w - 9) {
		int idx = (g->press_y - pane.y + list_scroll.offset) /
			LIST_ROW_H;

		if (idx >= 0 && idx < LIST_ROWS) {
			char b[64];
			unsigned int n = 0;

			list_selected = idx;
			ngui_scroll_ensure_visible(&list_scroll,
						   idx * LIST_ROW_H,
						   LIST_ROW_H);
			s_append(b, &n, "row ", sizeof(b));
			s_append_uint(b, &n, (unsigned int)(idx + 1),
				      sizeof(b));
			s_append(b, &n, " - ", sizeof(b));
			s_append(b, &n, list_items[idx], sizeof(b));
			event_set(b);
			ngui_invalidate(g);
		}
	}

	for (i = 0; i < LIST_ROWS; i++) {
		int ry = pane.y + i * LIST_ROW_H - list_scroll.offset;
		struct ngui_rect row = rect_make(pane.x, ry, pane.w - 9,
						 LIST_ROW_H);
		char label[64];
		unsigned int n = 0;
		unsigned short bg;
		int hover = g->inside &&
			ngui_rect_contains(&pane, g->mouse_x, g->mouse_y) &&
			g->mouse_x < pane.x + pane.w - 9 &&
			(g->mouse_y - pane.y + list_scroll.offset) /
				LIST_ROW_H == i;

		if (i == list_selected)
			bg = g->theme.selection;
		else if (hover)
			bg = g->theme.button_hot;
		else
			bg = (i & 1) ? g->theme.panel : g->theme.bg;
		ngui_fill_rect(g, row, bg);
		ngui_icon(g, rect_make(pane.x + 8, ry + 4, 16, 16),
			  (i % 3) == 2 ? NGUI_ICON_DISK : NGUI_ICON_FILE,
			  i == list_selected ? g->theme.text :
					       g->theme.icon_file);
		s_append_uint(label, &n, (unsigned int)(i + 1), sizeof(label));
		if (i + 1 < 10)
			s_append(label, &n, "  ", sizeof(label));
		else
			s_append(label, &n, " ", sizeof(label));
		s_append(label, &n, list_items[i], sizeof(label));
		ngui_text_ellipsis(g, rect_make(pane.x + 32, ry + 4,
						pane.w - 9 - 44, 16),
				   pane.x + 32, ry + 4, label,
				   i == list_selected ? g->theme.text :
							g->theme.text_dim);
	}
	ngui_scroll_end(g, pane, &list_scroll);

	help_x = x + pane_w + 24;
	help_w = card.x + card.w - CARD_PAD - help_x;
	if (help_w >= 150) {
		char sel[32];
		unsigned int n = 0;
		int hy = card.y + 40;
		int hh = pane_h;

		ngui_fill_rect(g, rect_make(help_x, hy, help_w, hh),
			       g->theme.bg);
		ngui_frame(g, rect_make(help_x, hy, help_w, hh),
			   g->theme.border);
		ngui_text(g, help_x + 10, hy + 9, "ngui_scroll",
			  g->theme.accent);
		ngui_text(g, help_x + 10, hy + 36, "12 rows x 24 px",
			  g->theme.text_dim);
		ngui_text(g, help_x + 10, hy + 54, "view_h follows pane",
			  g->theme.text_dim);
		ngui_text(g, help_x + 10, hy + 72, "drag thumb or track",
			  g->theme.text_dim);
		ngui_text(g, help_x + 10, hy + 90, "clip hides overflow",
			  g->theme.text_dim);
		ngui_text(g, help_x + 10, hy + 108, "ensure_visible()",
			  g->theme.text_dim);
		n = 0;
		s_append(sel, &n, "selected: ", sizeof(sel));
		s_append_uint(sel, &n, (unsigned int)(list_selected + 1),
			      sizeof(sel));
		ngui_text(g, help_x + 10, hy + 126, sel,
			  g->theme.accent);
	}
}

static void draw_layout(struct ngui *g, struct ngui_rect card)
{
	int x = card.x + CARD_PAD;
	int y = card.y + 16;
	int ow = card.w - 2 * CARD_PAD;
	struct ngui_rect outer, top, bottom, left, right, center;
	int hx;

	if (ow > 520)
		ow = 520;
	outer = rect_make(x, y, ow, card.h - 32);
	if (outer.w < 200 || outer.h < 80)
		return;

	top = ngui_cut_top(&outer, 30);
	bottom = ngui_cut_bottom(&outer, 30);
	left = ngui_cut_left(&outer, 120);
	right = ngui_cut_right(&outer, 120);
	center = outer;

	ngui_fill_rect(g, top, g->theme.selection);
	ngui_frame(g, top, g->theme.border);
	center_text(g, top, "ngui_cut_top(r, 30)", g->theme.text);

	ngui_fill_rect(g, bottom, g->theme.button);
	ngui_frame(g, bottom, g->theme.border);
	center_text(g, bottom, "ngui_cut_bottom(r, 30)", g->theme.text);

	ngui_fill_rect(g, left, g->theme.panel_alt);
	ngui_frame(g, left, g->theme.border);
	center_text(g, left, "cut_left", g->theme.text);

	ngui_fill_rect(g, right, g->theme.panel_alt);
	ngui_frame(g, right, g->theme.border);
	center_text(g, right, "cut_right", g->theme.text);

	ngui_fill_rect(g, center, g->theme.bg);
	ngui_frame(g, center, g->theme.border);
	center_text(g, center, "leftover rect", g->theme.text_dim);

	hx = x + ow + 24;
	if (card.x + card.w - CARD_PAD - hx < 150)
		return;
	ngui_text(g, hx, card.y + 16, "layout helpers",
		  g->theme.accent);
	ngui_text(g, hx, card.y + 40, "ngui_cut_top(r, h)",
		  g->theme.text_dim);
	ngui_text(g, hx, card.y + 58, "ngui_cut_bottom(r, h)",
		  g->theme.text_dim);
	ngui_text(g, hx, card.y + 76, "ngui_cut_left(r, w)",
		  g->theme.text_dim);
	ngui_text(g, hx, card.y + 94, "ngui_cut_right(r, w)",
		  g->theme.text_dim);
	ngui_text(g, hx, card.y + 112, "ngui_inset(r, n)",
		  g->theme.text_dim);
	ngui_text(g, hx, card.y + 136, "Each cut shrinks r and",
		  g->theme.border);
	ngui_text(g, hx, card.y + 154, "returns the cut piece.",
		  g->theme.border);
}

static void draw_body(struct ngui *g, struct ngui_rect body)
{
	int x = body.x + PAGE_PAD;
	int w = body.w - PAGE_PAD * 2 - 9;
	int i;

	page.content_h = sec_content_h;
	ngui_fill_rect(g, body, g->theme.bg);
	ngui_scroll_begin(g, body, &page);

	for (i = 0; i < SEC_COUNT; i++) {
		int sy = body.y - page.offset + sec_y[i];
		struct ngui_rect card;

		if (sy + SEC_HEAD_H + sec_card_h[i] < body.y - 4 ||
		    sy > body.y + body.h + 4)
			continue;
		draw_section_head(g, x, sy, w, i);
		card = rect_make(x, sy + SEC_HEAD_H, w, sec_card_h[i]);
		switch (i) {
		case SEC_TEXT:
			draw_typography(g, card);
			break;
		case SEC_SHAPES:
			draw_shapes(g, card);
			break;
		case SEC_THEME:
			draw_theme(g, card);
			break;
		case SEC_ICONS:
			draw_icons(g, card);
			break;
		case SEC_BUTTONS:
			draw_buttons(g, card);
			break;
		case SEC_INPUTS:
			draw_inputs(g, card);
			break;
		case SEC_SCROLL:
			draw_scroll(g, card);
			break;
		case SEC_LAYOUT:
			draw_layout(g, card);
			break;
		default:
			break;
		}
	}
	ngui_scroll_end(g, body, &page);
}

static void draw_status(struct ngui *g, struct ngui_rect r)
{
	char left[160];
	char right[64];
	unsigned int n = 0, m = 0;
	int rw, rx;

	ngui_fill_rect(g, r, g->theme.panel);
	ngui_fill_rect(g, rect_make(r.x, r.y, r.w, 1), g->theme.border);

	if (g->inside) {
		s_append(right, &m, "x ", sizeof(right));
		s_append_uint(right, &m, (unsigned int)g->mouse_x,
			      sizeof(right));
		s_append(right, &m, "  y ", sizeof(right));
		s_append_uint(right, &m, (unsigned int)g->mouse_y,
			      sizeof(right));
	} else {
		s_append(right, &m, "pointer outside", sizeof(right));
	}
	s_append(right, &m, "   ", sizeof(right));
	s_append_uint(right, &m, (unsigned int)page_percent(),
		      sizeof(right));
	s_append(right, &m, "%", sizeof(right));

	rw = (int)ngui_text_width_ui(g, right);
	rx = r.x + r.w - rw - 10;
	ngui_text(g, rx, r.y + 4, right, g->theme.text_dim);

	if (ngui_focus(&ui)) {
		s_append(left, &n, "editing ", sizeof(left));
		s_append(left, &n, focus_name(), sizeof(left));
		s_append(left, &n, "  |  ", sizeof(left));
	}
	if (event_text[0])
		s_append(left, &n, event_text, sizeof(left));
	else
		s_append(left, &n, "Ready - every ngui control on one page",
			 sizeof(left));

	if (rx - (r.x + 10) > 40)
		ngui_text_ellipsis(g, rect_make(r.x + 10, r.y,
						rx - r.x - 20, r.h),
				   r.x + 10, r.y + 4, left, g->theme.text);
}

static void draw(struct ngui *g)
{
	struct ngui_rect all = {0, 0, g->w, g->h};
	struct ngui_rect header, nav, status, body;

	if (g->w < 420 || g->h < 300) {
		ngui_fill_rect(g, all, g->theme.bg);
		ngui_text(g, 16, 16, "The gallery needs a larger window.",
			  g->theme.text);
		ngui_text(g, 16, 38, "Resize the window to continue.",
			  g->theme.text_dim);
		return;
	}

	header = ngui_cut_top(&all, HEADER_H);
	nav = ngui_cut_top(&all, NAV_H);
	status = ngui_cut_bottom(&all, STATUS_H);
	body = all;

	draw_header(g, header);
	draw_nav(g, nav);
	draw_body(g, body);
	draw_status(g, status);
}

static void handle_keys(struct ngui *g)
{
	int k;

	if (ngui_focus(g)) {
		/* A focused field consumed its keys; Ctrl+C is never consumed. */
		while ((k = ngui_key(g)) != 0)
			if (k == 0x03)
				quit = 1;
		return;
	}

	k = ngui_key_peek(g);
	if (k == NGUI_KEY_PAGE_UP || k == NGUI_KEY_PAGE_DOWN) {
		if (ngui_scroll_keys(g, &page)) {
			event_set(k == NGUI_KEY_PAGE_UP ?
				  "page up" : "page down");
			ngui_invalidate(g);
		}
		return;
	}

	while ((k = ngui_key(g)) != 0) {
		switch (k) {
		case 0x03:              /* Ctrl+C */
		case NGUI_KEY_ESC:
			quit = 1;
			return;
		case NGUI_KEY_UP:
			ngui_scroll_to(&page, page.offset - 36);
			ngui_invalidate(g);
			break;
		case NGUI_KEY_DOWN:
			ngui_scroll_to(&page, page.offset + 36);
			ngui_invalidate(g);
			break;
		case NGUI_KEY_HOME:
			ngui_scroll_to(&page, 0);
			event_set("jumped to top");
			ngui_invalidate(g);
			break;
		case NGUI_KEY_END:
			ngui_scroll_to(&page, page.content_h - page.view_h);
			event_set("jumped to the end");
			ngui_invalidate(g);
			break;
		case NGUI_KEY_TAB:
			ngui_set_focus(g, ID_IN_NAME);
			ngui_scroll_to(&page, sec_y[SEC_INPUTS]);
			event_set("focused the name field");
			ngui_invalidate(g);
			break;
		default:
			break;
		}
	}
}

void _start(const char *arg)
{
	struct win_info wi;

	(void)arg;
	if (u_win_create(START_W, START_H, "ngui gallery", &wi) != 0) {
		u_puts("gallery: no graphics window\n");
		u_exit(1);
	}

	ngui_init(&ui, (unsigned short *)wi.fb, (int)wi.width,
		  (int)wi.height);
	ngui_font_attach(&ui);

	name_in.placeholder = "Type a name";
	email_in.placeholder = "you@example.com";
	events_in.placeholder = "Type and press Return";
	disabled_in.placeholder = "Cannot edit";
	disabled_in.disabled = 1;
	ngui_input_set(&disabled_in, "Read-only value");

	gallery_layout();
	page.content_h = sec_content_h;
	page.view_h = START_H - HEADER_H - NAV_H - STATUS_H;
	ngui_input_set(&email_in, "ada@nebos.org");
	s_copy(input_feedback, "No events yet - edit a field.",
		sizeof(input_feedback));
	s_copy(event_text, "Ready - every ngui control on one page",
		sizeof(event_text));

	for (;;) {
		int want = ngui_begin(&ui);

		if (quit)
			break;
		if (!want) {
			ngui_end_ex(&ui, 0);
			u_sleep_ms(10);
			continue;
		}
		draw(&ui);
		handle_keys(&ui);
		if (quit)
			break;
		ngui_end(&ui);
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(0);
}
