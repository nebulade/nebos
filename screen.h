/* Text screen: a grid of character cells (char + VGA attribute) plus a cursor.
   Pure cell-buffer logic — no framebuffer access. The window manager renders
   these screens into windows. */
#ifndef SCREEN_H
#define SCREEN_H

#define SCREEN_COLS 64
#define SCREEN_ROWS 20

struct text_screen {
	unsigned short cells[SCREEN_COLS * SCREEN_ROWS];
	unsigned int cols;
	unsigned int rows;
	unsigned int cursor;          /* linear index 0..cols*rows-1 */
	unsigned int cursor_attr;
	unsigned int cursor_visible;
	int dirty;                    /* content changed since last WM render */
	int scrolled;                 /* one-shot, replaces io_did_scroll */
};

void screen_init(struct text_screen *s);
void screen_clear(struct text_screen *s);
void screen_putchar(struct text_screen *s, char c);
void screen_putstr(struct text_screen *s, const char *str);
void screen_puthex(struct text_screen *s, unsigned int n);
unsigned int screen_get_cursor(const struct text_screen *s);
void screen_set_cursor(struct text_screen *s, unsigned int pos);
void screen_put_at(struct text_screen *s, unsigned int pos, unsigned short value);
int screen_did_scroll(struct text_screen *s);

#endif
