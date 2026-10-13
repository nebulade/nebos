/* Text screen: a grid of character cells (char + VGA attribute) plus a cursor.
   Pure cell-buffer logic — no framebuffer access. The window manager renders
   these screens into windows. */
#ifndef SCREEN_H
#define SCREEN_H

#define SCREEN_COLS 64
#define SCREEN_ROWS 32

struct text_screen {
	unsigned short cells[SCREEN_COLS * SCREEN_ROWS];
	unsigned int cols;
	unsigned int rows;
	unsigned int cursor;          /* linear index 0..cols*rows-1 */
	unsigned int cursor_attr;
	unsigned int cursor_visible;
	/* Pending damage: the inclusive cell block [dmg_col0..dmg_col1] x
	   [dmg_row0..dmg_row1] that changed since the last render, so the WM
	   repaints a keystroke's cell instead of the whole window content.
	   Only the helpers below update it -- screen_damage_block() reports it
	   and screen_clear_damage() drops it after rendering. */
	int dirty;                    /* content changed since last WM render */
	unsigned int dmg_col0, dmg_col1;
	unsigned int dmg_row0, dmg_row1;
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

/* Pending damage as a cell block: returns 1 and fills col0, row0, cols and
   rows when something changed, 0 when the screen is clean. */
int screen_damage_block(const struct text_screen *s, unsigned int *col0,
		unsigned int *row0, unsigned int *cols, unsigned int *rows);
void screen_clear_damage(struct text_screen *s);
/* Mark the cell the cursor sits in (a blink or a cursor move only changes
   that cell, but both the old and the new position must be repainted). */
void screen_damage_cursor(struct text_screen *s);

#endif
