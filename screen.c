/* Text screen: cell-buffer text output (no framebuffer access). */

#include "screen.h"
#include "netlog.h"

#define SCREEN_ATTR 0x0A  /* light green on black */

void screen_init(struct text_screen *s)
{
	s->cols = SCREEN_COLS;
	s->rows = SCREEN_ROWS;
	s->cursor = 0;
	s->cursor_attr = (SCREEN_ATTR << 8);
	s->cursor_visible = 0;
	s->dirty = 1;
	s->scrolled = 0;
	screen_clear(s);
}

void screen_clear(struct text_screen *s)
{
	unsigned int i, n = s->cols * s->rows;
	for (i = 0; i < n; i++)
		s->cells[i] = (unsigned short)((SCREEN_ATTR << 8) | ' ');
	s->cursor = 0;
	s->dirty = 1;
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
	s->dirty = 1;
}

void screen_putchar(struct text_screen *s, char c)
{
	unsigned int ncols = s->cols;
	unsigned int total = s->cols * s->rows;

	netlog_char(c);   /* terminal text also goes to the netlog mirror */

	if (c == '\n') {
		s->cursor = (s->cursor / ncols + 1) * ncols;
		if (s->cursor >= total) {
			screen_scroll_up(s);
			s->cursor = total - ncols;
			s->scrolled = 1;
		}
		s->cursor_attr = (SCREEN_ATTR << 8);
		s->dirty = 1;
		return;
	}
	if (c == '\b') {
		if (s->cursor > 0) {
			s->cursor--;
			s->cells[s->cursor] = (unsigned short)((SCREEN_ATTR << 8) | ' ');
			s->dirty = 1;
		}
		return;
	}
	if (s->cursor < total) {
		s->cells[s->cursor] = (unsigned short)(s->cursor_attr | (unsigned char)c);
		s->cursor++;
		s->cursor_attr = (SCREEN_ATTR << 8);
		s->dirty = 1;
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
	s->cursor = pos;
	s->cursor_attr = (SCREEN_ATTR << 8);
}

void screen_put_at(struct text_screen *s, unsigned int pos, unsigned short value)
{
	if (pos < s->cols * s->rows) {
		s->cells[pos] = value;
		s->dirty = 1;
	}
}

int screen_did_scroll(struct text_screen *s)
{
	if (!s->scrolled)
		return 0;
	s->scrolled = 0;
	return 1;
}
