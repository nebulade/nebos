/* Text screen: cell-buffer text output (no framebuffer access). */

#include "screen.h"
#include "netlog.h"

#define SCREEN_ATTR 0x0A  /* light green on black */

/* ---- Damage tracking ----
 *
 * The WM repaints only the cell block that changed, so every mutation of the
 * cell buffer -- or of the cursor, which is drawn as an overlay -- has to mark
 * it: screen_damage_cell() grows the block around a cell, screen_damage_all()
 * covers the whole grid (a scroll or a clear) and screen_damage_cursor() marks
 * the cell the cursor sits in.
 */
static void screen_damage_cell(struct text_screen *s, unsigned int col, unsigned int row)
{
	s->damage_seq++;
	if (!s->dirty) {
		s->dirty = 1;
		s->dmg_col0 = s->dmg_col1 = col;
		s->dmg_row0 = s->dmg_row1 = row;
		return;
	}
	if (col < s->dmg_col0)
		s->dmg_col0 = col;
	if (col > s->dmg_col1)
		s->dmg_col1 = col;
	if (row < s->dmg_row0)
		s->dmg_row0 = row;
	if (row > s->dmg_row1)
		s->dmg_row1 = row;
}

static void screen_damage_all(struct text_screen *s)
{
	s->damage_seq++;
	s->dirty = 1;
	s->dmg_col0 = 0;
	s->dmg_col1 = s->cols - 1;
	s->dmg_row0 = 0;
	s->dmg_row1 = s->rows - 1;
}

void screen_damage_cursor(struct text_screen *s)
{
	unsigned int pos = s->cursor;

	if (pos >= s->cols * s->rows) {
		screen_damage_all(s);
		return;
	}
	screen_damage_cell(s, pos % s->cols, pos / s->cols);
}

int screen_damage_block(const struct text_screen *s, unsigned int *col0,
		unsigned int *row0, unsigned int *cols, unsigned int *rows)
{
	if (!s->dirty)
		return 0;
	*col0 = s->dmg_col0;
	*row0 = s->dmg_row0;
	*cols = s->dmg_col1 - s->dmg_col0 + 1;
	*rows = s->dmg_row1 - s->dmg_row0 + 1;
	return 1;
}

void screen_clear_damage(struct text_screen *s)
{
	s->dirty = 0;
}

void screen_init(struct text_screen *s)
{
	s->cols = SCREEN_COLS;
	s->rows = SCREEN_ROWS;
	s->cursor = 0;
	s->cursor_attr = (SCREEN_ATTR << 8);
	s->cursor_visible = 0;
	s->scrolled = 0;
	s->damage_seq = 0;
	screen_clear(s);
}

void screen_clear(struct text_screen *s)
{
	unsigned int i, n = s->cols * s->rows;
	for (i = 0; i < n; i++)
		s->cells[i] = (unsigned short)((SCREEN_ATTR << 8) | ' ');
	s->cursor = 0;
	screen_damage_all(s);
	s->scrolled = 0;
}

static void screen_scroll_up(struct text_screen *s)
{
	unsigned int ncols = s->cols;
	unsigned int total = s->cols * s->rows;
	unsigned int i;
	for (i = 0; i < total - ncols; i++)
		s->cells[i] = s->cells[i + ncols];
	for (; i < total; i++)
		s->cells[i] = (unsigned short)((SCREEN_ATTR << 8) | ' ');
	screen_damage_all(s);
}

void screen_putchar(struct text_screen *s, char c)
{
	unsigned int ncols = s->cols;
	unsigned int total = s->cols * s->rows;

	netlog_char(c);   /* terminal text also goes to the netlog mirror */

	if (c == '\n') {
		screen_damage_cursor(s);      /* the cursor leaves this cell */
		s->cursor = (s->cursor / ncols + 1) * ncols;
		if (s->cursor >= total) {
			screen_scroll_up(s);
			s->cursor = total - ncols;
			s->scrolled = 1;
		}
		s->cursor_attr = (SCREEN_ATTR << 8);
		screen_damage_cursor(s);
		return;
	}
	if (c == '\b') {
		if (s->cursor > 0) {
			screen_damage_cursor(s);
			s->cursor--;
			s->cells[s->cursor] = (unsigned short)((SCREEN_ATTR << 8) | ' ');
			screen_damage_cursor(s);
		}
		return;
	}
	if (s->cursor < total) {
		screen_damage_cursor(s);      /* the written cell + the cursor */
		s->cells[s->cursor] = (unsigned short)(s->cursor_attr | (unsigned char)c);
		s->cursor++;
		s->cursor_attr = (SCREEN_ATTR << 8);
		screen_damage_cursor(s);      /* the cursor moved on */
	}
}

void screen_putstr(struct text_screen *s, const char *str)
{
	while (*str)
		screen_putchar(s, *str++);
}

void screen_puthex(struct text_screen *s, unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[9];
	unsigned int i = 8;
	buf[8] = '\0';
	do {
		buf[--i] = hex[n & 0xF];
		n >>= 4;
	} while (i > 0);
	screen_putstr(s, buf);
}

unsigned int screen_get_cursor(const struct text_screen *s)
{
	return s->cursor;
}

void screen_set_cursor(struct text_screen *s, unsigned int pos)
{
	if (pos >= s->cols * s->rows)
		return;
	screen_damage_cursor(s);      /* erase the old cursor cell */
	s->cursor = pos;
	s->cursor_attr = (SCREEN_ATTR << 8);
	screen_damage_cursor(s);
}

void screen_put_at(struct text_screen *s, unsigned int pos, unsigned short value)
{
	if (pos < s->cols * s->rows) {
		screen_damage_cell(s, pos % s->cols, pos / s->cols);
		s->cells[pos] = value;
	}
}

int screen_did_scroll(struct text_screen *s)
{
	if (!s->scrolled)
		return 0;
	s->scrolled = 0;
	return 1;
}
