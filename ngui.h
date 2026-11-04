/* ngui: tiny immediate-mode GUI toolkit for ring-3 canvas windows.
 *
 * The toolkit owns no memory: the app passes its framebuffer and keeps the
 * state structs for text fields and scroll panes.  Everything is RGB565 and
 * ASCII, using the kernel's 8x16 font.  See ucmd_files.c for a user. */
#ifndef NGUI_H
#define NGUI_H

#include "syscall.h"
#include "wm_abi.h"

#define NGUI_INPUT_MAX 128

#define NGUI_INPUT_CHANGED 1
#define NGUI_INPUT_SUBMIT  2
#define NGUI_INPUT_CANCEL  4

/* Key codes from io.c.  Arrows are 0x100..; the page/home/end keys are
 * produced by the extended PS/2 path. */
#define NGUI_KEY_ESC        0x1B
#define NGUI_KEY_BACKSPACE  0x08
#define NGUI_KEY_TAB        0x09
#define NGUI_KEY_LEFT       0x100
#define NGUI_KEY_RIGHT      0x101
#define NGUI_KEY_UP         0x102
#define NGUI_KEY_DOWN       0x103
#define NGUI_KEY_PAGE_UP    0x104
#define NGUI_KEY_PAGE_DOWN  0x105
#define NGUI_KEY_HOME       0x106
#define NGUI_KEY_END        0x107
#define NGUI_KEY_DELETE     0x108
#define NGUI_KEY_CTRL_LEFT  0x109
#define NGUI_KEY_CTRL_RIGHT 0x10A
#define NGUI_KEY_CTRL_DELETE 0x10B
#define NGUI_KEY_CTRL_BACKSPACE 0x10C

#define NGUI_RGB(r, g, b) \
	((unsigned short)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

struct ngui_rect {
	int x, y, w, h;
};

struct ngui_theme {
	unsigned short bg;
	unsigned short panel;
	unsigned short panel_alt;
	unsigned short border;
	unsigned short text;
	unsigned short text_dim;
	unsigned short accent;
	unsigned short selection;
	unsigned short button;
	unsigned short button_hot;
	unsigned short button_down;
	unsigned short icon_folder;
	unsigned short icon_file;
	unsigned short icon_disk;
	unsigned short error;
};

extern const struct ngui_theme ngui_theme_dark;

enum ngui_icon {
	NGUI_ICON_FOLDER,
	NGUI_ICON_FILE,
	NGUI_ICON_DISK,
	NGUI_ICON_UP,
	NGUI_ICON_HOME,
	NGUI_ICON_REFRESH,
	NGUI_ICON_PLUS,
	NGUI_ICON_CHEVRON_LEFT,
	NGUI_ICON_CHEVRON_RIGHT
};

struct ngui_input {
	char text[NGUI_INPUT_MAX];
	unsigned int len;
	unsigned int cursor;
	unsigned int scroll;          /* first visible character */
	unsigned int anchor;          /* mouse selection anchor (== cursor: none) */
	int dragging;                 /* a press in the field is extending a selection */
	const char *placeholder;
	unsigned int last_click_ms;   /* previous press, for double-click select-all */
};

struct ngui_scroll {
	int offset;                   /* pixels scrolled from the top */
	int content_h;
	int view_h;
	int dragging;
	int drag_dy;
};

struct ngui {
	unsigned short *px;
	int w, h;
	struct ngui_theme theme;

	/* Current pointer state, content-relative. */
	int mouse_x, mouse_y;
	unsigned int buttons;
	int inside;
	int focused;

	/* Press/release edges collected by ngui_begin(). */
	unsigned int pressed;
	unsigned int released;
	int press_x, press_y;
	int release_x, release_y;
	int clicked;
	int focus_claimed;

	unsigned int hot;
	unsigned int active;
	unsigned int focus;

	struct ngui_rect clip[4];
	int nclip;

	int dirty;
	unsigned int last_blink;
	int caret_on;

	int keys[32];
	unsigned int key_head, key_tail;

	int last_mouse_x, last_mouse_y;
	int last_inside, last_focused;
};

void ngui_init(struct ngui *ui, unsigned short *fb, int w, int h);
int  ngui_begin(struct ngui *ui);       /* 1 = app should redraw */
void ngui_end(struct ngui *ui);
/* Same bookkeeping, but only publish the canvas when `present` is nonzero.
 * Lets an app poll input without copying an unchanged frame every loop. */
void ngui_end_ex(struct ngui *ui, int present);
void ngui_invalidate(struct ngui *ui);
void ngui_set_focus(struct ngui *ui, unsigned int id);
unsigned int ngui_focus(struct ngui *ui);
int  ngui_key(struct ngui *ui);
int  ngui_key_peek(struct ngui *ui);

/* Layout: cut a rectangle off one side of `r` and return the piece. */
struct ngui_rect ngui_cut_top(struct ngui_rect *r, int h);
struct ngui_rect ngui_cut_bottom(struct ngui_rect *r, int h);
struct ngui_rect ngui_cut_left(struct ngui_rect *r, int w);
struct ngui_rect ngui_cut_right(struct ngui_rect *r, int w);
void ngui_inset(struct ngui_rect *r, int n);
int  ngui_rect_contains(const struct ngui_rect *r, int x, int y);

/* Primitives.  All drawing is clipped to the current clip stack and canvas. */
void ngui_push_clip(struct ngui *ui, struct ngui_rect r);
void ngui_pop_clip(struct ngui *ui);
void ngui_fill_rect(struct ngui *ui, struct ngui_rect r, unsigned short color);
void ngui_frame(struct ngui *ui, struct ngui_rect r, unsigned short color);
void ngui_line(struct ngui *ui, int x0, int y0, int x1, int y1,
	       unsigned short color);
void ngui_text(struct ngui *ui, int x, int y, const char *s,
	       unsigned short color);
void ngui_text_n(struct ngui *ui, int x, int y, const char *s,
		 unsigned int n, unsigned short color);
void ngui_text_styled(struct ngui *ui, int x, int y, const char *s,
		      unsigned int n, unsigned short color, int scale, int bold);
void ngui_text_rot180(struct ngui *ui, int x, int y, const char *s,
		      unsigned int n, unsigned short color);
void ngui_text_ellipsis(struct ngui *ui, struct ngui_rect r, int x, int y,
			const char *s, unsigned short color);
unsigned int ngui_text_width(const char *s);
unsigned int ngui_text_width_n(const char *s, unsigned int n, int scale);
void ngui_icon(struct ngui *ui, struct ngui_rect r, enum ngui_icon icon,
	       unsigned short color);

/* Widgets.  IDs must be stable across frames. */
int ngui_button(struct ngui *ui, unsigned int id, struct ngui_rect r,
		const char *label);
int ngui_menu_item(struct ngui *ui, unsigned int id, struct ngui_rect r,
		   const char *label, const char *shortcut, int checked);
int ngui_icon_button(struct ngui *ui, unsigned int id, struct ngui_rect r,
		     enum ngui_icon icon);
int ngui_text_input(struct ngui *ui, unsigned int id, struct ngui_rect r,
		    struct ngui_input *in);
void ngui_input_set(struct ngui_input *in, const char *s);

/* Scrolling pane: call begin, draw the content with scroll->offset, end. */
void ngui_scroll_begin(struct ngui *ui, struct ngui_rect r,
		       struct ngui_scroll *s);
void ngui_scroll_end(struct ngui *ui, struct ngui_rect r,
		     struct ngui_scroll *s);
void ngui_scroll_to(struct ngui_scroll *s, int offset);
void ngui_scroll_ensure_visible(struct ngui_scroll *s, int y, int h);
int  ngui_scroll_keys(struct ngui *ui, struct ngui_scroll *s);

#endif
