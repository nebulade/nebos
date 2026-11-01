/* edit: small ring-3 text editor drawn into the terminal window's canvas.
 *
 * Controls: printable keys, Enter, Backspace and Tab edit the buffer; the
 * arrow keys move; Ctrl+S saves the file named by the first argument and
 * Ctrl+Q quits (when the buffer is modified, a second Ctrl+Q discards the
 * changes).  Ctrl+C still reaches the terminal and kills the command.
 *
 * A gfx session is the only key-input path available to a ring-3 command, so
 * the editor repaints the window canvas itself.  The kernel's 8x8 font is
 * linked into this ELF by the Makefile (see ucmd_edit.elf). */

#include "ucmd_lib.h"
#include "font_8x8.h"

#define EDIT_MAX       16384
#define EDIT_READ_MAX  (EDIT_MAX + 1)   /* one extra byte detects "too large" */
/* The FAT read path copies whole 512-byte sectors, so the last partial
 * sector can write up to 511 bytes past the requested length.  Keep that
 * slack inside the buffer instead of letting it hit the next (unmapped)
 * page.  It is never displayed or saved. */
#define EDIT_BUF_SIZE  (EDIT_READ_MAX + 512)
#define TAB_WIDTH      4
#define CELL_W         8
#define CELL_H         10
#define PATH_MAX       128
#define BASE_MAX       64
#define STATUS_MAX     96

/* Key codes produced by the kernel keyboard path (see io.h). */
#define KEY_ESC        0x1B
#define KEY_BACKSPACE  0x08
#define KEY_CTRL_Q     0x11
#define KEY_CTRL_S     0x13
#define KEY_LEFT       0x100
#define KEY_RIGHT      0x101
#define KEY_UP         0x102
#define KEY_DOWN       0x103

#define RGB565(r, g, b) \
	((unsigned short)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const unsigned short col_bg        = RGB565(0, 0, 0);
static const unsigned short col_fg        = RGB565(170, 170, 170);
static const unsigned short col_hint_bg   = RGB565(32, 32, 48);
static const unsigned short col_hint_fg   = RGB565(85, 255, 255);
static const unsigned short col_status_bg = RGB565(0, 0, 170);
static const unsigned short col_status_fg = RGB565(255, 255, 255);
static const unsigned short col_cursor    = RGB565(255, 255, 255);
static const unsigned short col_cursor_fg = RGB565(0, 0, 0);

static char text[EDIT_BUF_SIZE];
static unsigned int len;
static unsigned int cur;
static char path[PATH_MAX];
static char base_name[BASE_MAX];
static unsigned int dirty;
static unsigned int scroll_top;
static unsigned int xoff;
static int quit_armed;
static char status_msg[STATUS_MAX];
static unsigned int status_until;

static unsigned short *fb;
static unsigned int fb_w, fb_h;
static unsigned int cols, rows, text_rows;

/* ---- tiny string and drawing helpers (no libc) ---- */

static unsigned int append_str(char *dst, unsigned int n, const char *s,
			       unsigned int max)
{
	while (*s && n + 1 < max)
		dst[n++] = *s++;
	dst[n] = '\0';
	return n;
}

static unsigned int append_uint(char *dst, unsigned int n, unsigned int v,
				unsigned int max)
{
	char tmp[11];
	unsigned int i = 0, j;

	do {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && i < sizeof(tmp));

	for (j = 0; j < i && n + 1 < max; j++)
		dst[n++] = tmp[i - 1 - j];
	dst[n] = '\0';
	return n;
}

static void fill_rect(unsigned int x, unsigned int y, unsigned int w,
		      unsigned int h, unsigned short color)
{
	unsigned int row, col;

	if (x >= fb_w || y >= fb_h)
		return;
	if (w > fb_w - x)
		w = fb_w - x;
	if (h > fb_h - y)
		h = fb_h - y;
	for (row = 0; row < h; row++) {
		unsigned short *p = fb + (y + row) * fb_w + x;

		for (col = 0; col < w; col++)
			p[col] = color;
	}
}

/* Draw the foreground pixels of one glyph; the background is left alone. */
static void draw_glyph_at(unsigned int x, unsigned int y, unsigned char c,
			  unsigned short fg)
{
	const unsigned char *g = &font_8x8[(unsigned int)c * FONT_8X8_ROWS];
	unsigned int row, col;

	if (x >= fb_w)
		return;
	for (row = 0; row < FONT_8X8_ROWS; row++) {
		unsigned int py = y + row;
		unsigned short *p;
		unsigned char bits;

		if (py >= fb_h)
			break;
		p = fb + py * fb_w + x;
		bits = g[row];
		for (col = 0; col < 8 && x + col < fb_w; col++)
			if (bits & (0x80u >> col))
				p[col] = fg;
	}
}

static void draw_string(unsigned int x, unsigned int y, const char *s,
			unsigned int max_chars, unsigned short fg)
{
	unsigned int i;

	for (i = 0; s[i] && i < max_chars; i++)
		draw_glyph_at(x + i * CELL_W, y, (unsigned char)s[i], fg);
}

static void set_msg(const char *s, unsigned int ms)
{
	append_str(status_msg, 0, s, sizeof(status_msg));
	status_until = u_ticks_ms() + ms;
	if (!status_until)
		status_until = 1;
}

/* ---- buffer / cursor geometry ---- */

static unsigned int cursor_line(void)
{
	unsigned int i, line = 0;

	for (i = 0; i < cur; i++)
		if (text[i] == '\n')
			line++;
	return line;
}

static unsigned int line_start_for(unsigned int line)
{
	unsigned int i, l = 0, start = 0;

	if (line == 0)
		return 0;
	for (i = 0; i < len; i++) {
		if (text[i] == '\n') {
			l++;
			start = i + 1;
			if (l == line)
				return start;
		}
	}
	return start;
}

static unsigned int line_end_from(unsigned int start)
{
	unsigned int e = start;

	while (e < len && text[e] != '\n')
		e++;
	return e;
}

static unsigned int display_col_at(unsigned int start, unsigned int pos)
{
	unsigned int i, d = 0;

	for (i = start; i < pos && i < len; i++) {
		if (text[i] == '\t')
			d = (d / TAB_WIDTH + 1) * TAB_WIDTH;
		else
			d++;
	}
	return d;
}

static void insert_char(char c)
{
	unsigned int i;

	if (len >= EDIT_MAX) {
		set_msg("Buffer full (16 KB max)", 1500);
		return;
	}
	i = len;
	while (i > cur) {
		text[i] = text[i - 1];
		i--;
	}
	text[cur] = c;
	cur++;
	len++;
	dirty = 1;
}

static void delete_char(void)
{
	unsigned int i;

	if (cur == 0)
		return;
	i = cur - 1;
	while (i + 1 < len) {
		text[i] = text[i + 1];
		i++;
	}
	cur--;
	len--;
	dirty = 1;
}

static void move_vertical(int dir)
{
	unsigned int line = cursor_line();
	unsigned int start = line_start_for(line);
	unsigned int col = cur - start;
	unsigned int target, target_end;

	if (dir < 0) {
		if (line == 0)
			return;
		target = line_start_for(line - 1);
	} else {
		unsigned int end = line_end_from(start);

		if (end >= len)
			return;          /* already on the last line */
		target = end + 1;
	}

	target_end = line_end_from(target);
	if (target_end - target < col)
		col = target_end - target;
	cur = target + col;
}

/* ---- file handling ---- */

static int parse_arg(const char *arg)
{
	unsigned int i = 0, n = 0, k, b = 0;

	if (!arg)
		return -1;
	while (arg[i] == ' ')
		i++;
	while (arg[i] && arg[i] != ' ' && n + 1 < sizeof(path))
		path[n++] = arg[i++];
	path[n] = '\0';
	if (n == 0)
		return -1;
	if (arg[i] && arg[i] != ' ')
		return -1;              /* filename does not fit */

	for (k = 0; path[k]; k++)
		if (path[k] == '/')
			b = k + 1;
	if (path[b] == '\0' && b > 0)
		b = 0;                  /* trailing slash: show the whole path */
	for (k = 0; path[b + k] && k + 1 < sizeof(base_name); k++)
		base_name[k] = path[b + k];
	base_name[k] = '\0';
	return 0;
}

/* 0 = loaded, 1 = new file (does not exist), -2 = too large, other <0 = err */
static int load_file(void)
{
	int n = u_syscall3(SYS_READ_FILE, (int)path, (int)text, EDIT_READ_MAX);
	unsigned int i, j;

	if (n >= 0) {
		if ((unsigned int)n >= EDIT_READ_MAX)
			return -2;
		len = (unsigned int)n;
		j = 0;
		for (i = 0; i < len; i++) {
			if (text[i] == '\r') {
				if (i + 1 < len && text[i + 1] == '\n')
					continue;
				text[j++] = '\n';
			} else {
				text[j++] = text[i];
			}
		}
		len = j;
		return 0;
	}
	if (n == SYS_ENOENT) {
		len = 0;
		return 1;
	}
	return n;
}

static const char *read_error(int r)
{
	switch (r) {
	case SYS_ENODEV:
		return "edit: no such device\n";
	case SYS_ENOFS:
		return "edit: no filesystem on device\n";
	case SYS_EIO:
		return "edit: disk read error\n";
	case SYS_EINVAL:
		return "edit: invalid path\n";
	default:
		return "edit: cannot read file\n";
	}
}

static void save_file(void)
{
	int r = u_syscall3(SYS_WRITE_FILE, (int)path, (int)text, (int)len);

	if (r == 0) {
		dirty = 0;
		set_msg("Saved", 1500);
		return;
	}
	switch (r) {
	case SYS_ENODEV:
		set_msg("Save failed: no such device", 2500);
		break;
	case SYS_ENOFS:
		set_msg("Save failed: no filesystem on device", 2500);
		break;
	case SYS_ENOENT:
		set_msg("Save failed: path not found", 2500);
		break;
	case SYS_EROFS:
		set_msg("Save failed: read-only filesystem", 2500);
		break;
	case SYS_EIO:
		set_msg("Save failed: disk write error", 2500);
		break;
	case SYS_EWRITE:
		set_msg("Save failed: disk full or refused", 2500);
		break;
	case SYS_EINVAL:
		set_msg("Save failed: invalid path", 2500);
		break;
	default:
		set_msg("Save failed", 2500);
		break;
	}
}

/* ---- rendering ---- */

static void draw_text_area(void)
{
	unsigned int row, pos = 0, i;

	fill_rect(0, 0, fb_w, text_rows * CELL_H, col_bg);

	/* Skip the lines above the viewport. */
	for (i = 0; i < scroll_top && pos < len; i++) {
		while (pos < len && text[pos] != '\n')
			pos++;
		if (pos < len)
			pos++;
	}

	for (row = 0; row < text_rows; row++) {
		unsigned int start = pos, end, disp = 0, col = 0, p;

		while (pos < len && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < len)
			pos++;

		for (p = start; p < end && col < cols; p++) {
			char c = text[p];

			if (c == '\t') {
				unsigned int next = (disp / TAB_WIDTH + 1) * TAB_WIDTH;

				while (disp < next && col < cols) {
					if (disp >= xoff)
						col++;
					disp++;
				}
			} else {
				if (disp >= xoff) {
					draw_glyph_at(col * CELL_W, row * CELL_H,
						      (unsigned char)c, col_fg);
					col++;
				}
				disp++;
			}
		}
	}
}

static void draw_cursor(unsigned int line, unsigned int disp)
{
	unsigned int row, col;

	if (line < scroll_top || disp < xoff)
		return;
	row = line - scroll_top;
	col = disp - xoff;
	if (row >= text_rows || col >= cols)
		return;

	fill_rect(col * CELL_W, row * CELL_H, CELL_W, CELL_H, col_cursor);
	if (cur < len && text[cur] != '\n' && text[cur] != '\t')
		draw_glyph_at(col * CELL_W, row * CELL_H,
			      (unsigned char)text[cur], col_cursor_fg);
}

static void draw_status(unsigned int line, unsigned int disp)
{
	char left[STATUS_MAX], right[48];
	unsigned int n, rn, max_left, x, y;
	const char *msg;

	if (quit_armed)
		msg = "Unsaved changes - Ctrl+Q again to discard, ESC to cancel";
	else if (status_until)
		msg = status_msg;
	else
		msg = "^S Save  ^Q Quit  Arrows move  Enter newline  Backspace delete";

	y = text_rows * CELL_H;
	fill_rect(0, y, fb_w, CELL_H, col_hint_bg);
	draw_string(0, y, msg, cols, col_hint_fg);

	y += CELL_H;
	fill_rect(0, y, fb_w, CELL_H, col_status_bg);

	n = append_str(left, 0, base_name, sizeof(left));
	if (dirty)
		n = append_str(left, n, " *", sizeof(left));

	rn = append_str(right, 0, "Ln ", sizeof(right));
	rn = append_uint(right, rn, line + 1, sizeof(right));
	rn = append_str(right, rn, " Col ", sizeof(right));
	rn = append_uint(right, rn, disp + 1, sizeof(right));

	if (rn >= cols) {
		draw_string(0, y, right, cols, col_status_fg);
		return;
	}
	max_left = cols - rn - 1;
	if (n > max_left)
		n = max_left;
	draw_string(0, y, left, n, col_status_fg);
	x = (cols - rn) * CELL_W;
	draw_string(x, y, right, rn, col_status_fg);
}

static void redraw(void)
{
	unsigned int line = cursor_line();
	unsigned int start = line_start_for(line);
	unsigned int disp = display_col_at(start, cur);

	if (line < scroll_top)
		scroll_top = line;
	else if (line >= scroll_top + text_rows)
		scroll_top = line - text_rows + 1;
	if (disp < xoff)
		xoff = disp;
	else if (disp >= xoff + cols)
		xoff = disp - cols + 1;

	draw_text_area();
	draw_cursor(line, disp);
	draw_status(line, disp);
	u_win_present();
}

/* ---- input ---- */

/* 1 = quit, 0 = keep editing */
static int handle_key(int k)
{
	if (quit_armed) {
		if (k == KEY_CTRL_Q)
			return 1;
		if (k == KEY_ESC) {
			quit_armed = 0;
			return 0;
		}
		quit_armed = 0;
	}

	if (k == KEY_CTRL_Q) {
		if (dirty) {
			quit_armed = 1;
			status_until = 0;
			return 0;
		}
		return 1;
	}

	switch (k) {
	case KEY_CTRL_S:
		save_file();
		break;
	case KEY_LEFT:
		if (cur > 0)
			cur--;
		break;
	case KEY_RIGHT:
		if (cur < len)
			cur++;
		break;
	case KEY_UP:
		move_vertical(-1);
		break;
	case KEY_DOWN:
		move_vertical(1);
		break;
	case KEY_BACKSPACE:
	case 0x7F:
		delete_char();
		break;
	case '\n':
	case '\r':
		insert_char('\n');
		break;
	case '\t':
		insert_char('\t');
		break;
	case KEY_ESC:
		status_until = 0;
		break;
	default:
		if (k >= 0x20 && k <= 0xFF)
			insert_char((char)k);
		break;
	}
	return 0;
}

void _start(const char *arg)
{
	struct win_info info;
	int r, new_file;

	if (parse_arg(arg) != 0) {
		u_puts("edit: usage edit <filename>\n");
		u_exit(1);
	}

	r = load_file();
	if (r == -2) {
		u_puts("edit: file too large (max 16 KB)\n");
		u_exit(1);
	}
	if (r < 0) {
		u_puts(read_error(r));
		u_exit(1);
	}
	new_file = (r == 1);

	if (u_win_create(512, 320, "edit", &info) != 0) {
		u_puts("edit: no graphics window\n");
		u_exit(1);
	}
	fb = (unsigned short *)info.fb;
	fb_w = info.width;
	fb_h = info.height;
	cols = fb_w / CELL_W;
	rows = fb_h / CELL_H;
	if (cols == 0 || rows < 2) {
		u_win_close();
		u_puts("edit: window too small\n");
		u_exit(1);
	}
	text_rows = rows - 2;

	cur = 0;
	scroll_top = 0;
	xoff = 0;
	dirty = 0;
	quit_armed = 0;
	status_until = 0;
	status_msg[0] = '\0';
	if (new_file)
		set_msg("New file - Ctrl+S saves", 2000);

	redraw();
	for (;;) {
		int quit = 0, got = 0, k;
		unsigned int now;

		while ((k = u_key_poll()) != 0) {
			got = 1;
			if (handle_key(k)) {
				quit = 1;
				break;
			}
		}
		if (quit)
			break;
		now = u_ticks_ms();
		if (status_until && (int)(now - status_until) >= 0) {
			status_until = 0;
			got = 1;
		}
		if (got)
			redraw();
		u_sleep_ms(15);
	}

	u_win_close();
	u_exit(0);
}
