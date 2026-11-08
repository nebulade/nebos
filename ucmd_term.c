/* Ring-3 terminal: owns a canvas window, a scrollback of text rows and the
 * shell.  Text commands write bytes to the tty; this process drains and
 * renders them.  Privileged kernel commands are spawned as kernel processes
 * (term_kcmd).
 *
 * The screen is the tail of a ring of display rows, so output that scrolls
 * off the top stays available behind a scrollbar.  Rows carry a "wrapped"
 * flag (the line continued because it did not fit), which lets a window
 * resize re-flow the whole log at the new width instead of clearing it. */

#include "ucmd_lib.h"
#include "uwm_lib.h"
#include "io.h"
#include "vfs.h"

#define MAX_COLS 128
#define MAX_ROWS 64
#define BUF_MAX 256
#define LINE_MAX 128
#define HIST_MAX 16

/* Scrollbar on the right edge of the canvas.  The text area is narrowed by
 * SB_W so the bar never covers a glyph; the bottom SB_BOTTOM pixels stay
 * free for the window manager's resize grip. */
#define SB_W 8
#define SB_BOTTOM 16

/* Total row storage (screen + scrollback).  Allocated from the per-process
 * heap; the fallback keeps a screen-only terminal working if that fails. */
#define SCROLL_ROWS_PREF 512
#define SCROLL_ROWS_MIN  128

#define POS_NONE 0xFFFFFFFFu

/* One display row.  `wrapped` marks a row that continues on the next one
 * because the logical line did not fit at the width it was written at. */
struct trow {
	unsigned short cells[MAX_COLS];
	unsigned int len;
	unsigned int wrapped;
};

/* A position captured before a reflow and mapped through it. */
struct pos_map {
	unsigned int abs;    /* absolute row index in the row log */
	unsigned int col;    /* column within that row */
	unsigned int start;  /* first row of the containing logical line */
	unsigned int off;    /* character offset inside that logical line */
	unsigned int dst;    /* destination row of the logical line start */
};

static struct win_info wi;
static struct usurf surf;
static int tty;
static unsigned short view_cells[MAX_COLS * MAX_ROWS];
static struct trow base_rows[MAX_ROWS];
static struct trow base_scratch[MAX_ROWS];
static struct trow *row_mem = base_rows;
static struct trow *row_scratch = base_scratch;
static unsigned int row_cap = MAX_ROWS;
static unsigned int row_first;    /* ring index of logical row 0 */
static unsigned int row_count;    /* valid rows (always >= rows) */
static unsigned int view;         /* rows scrolled back from the bottom */
static unsigned int cols = 64, rows = 32;
static unsigned int cx, cy;
static char line[LINE_MAX];
static unsigned int line_len;
static unsigned int line_abs, line_col;
static int hist_browse;
static char cwd[VFS_PATH_MAX];
static char path[VFS_PATH_MAX];
static char hist[HIST_MAX][LINE_MAX];
static unsigned int hist_count, hist_next;
static int fg_pid = -1;
static int dirty = 1;
static int dirty_full = 1;   /* the whole canvas changed */
static int dmg_valid;
static unsigned int dmg_y0, dmg_y1;  /* damage rows, inclusive */
static int cursor_on = 1;
static unsigned int last_blink;
static int sb_drag;
static int sb_grab_ofs;
static int wrap_pending;   /* the row is full; the next glyph wraps */

static void mark_full(void)
{
	dirty = 1;
	dirty_full = 1;
	dmg_valid = 0;
}

static void mark_row(unsigned int y)
{
	if (y >= rows)
		return;
	dirty = 1;
	if (dirty_full)
		return;
	if (!dmg_valid) {
		dmg_y0 = dmg_y1 = y;
		dmg_valid = 1;
	} else {
		if (y < dmg_y0)
			dmg_y0 = y;
		if (y > dmg_y1)
			dmg_y1 = y;
	}
}

/* ---- Row log ---- */

static struct trow *row_at(unsigned int i)
{
	return &row_mem[(row_first + i) % row_cap];
}

static struct trow *dst_row(unsigned int i)
{
	return &row_scratch[i % row_cap];
}

static unsigned int origin_row(void)
{
	return row_count - rows;
}

static void row_blank(struct trow *r)
{
	unsigned int i;

	for (i = 0; i < MAX_COLS; i++)
		r->cells[i] = 0x0A20;
	r->len = 0;
	r->wrapped = 0;
}

static void row_add_blank(void)
{
	struct trow *r;

	if (row_count < row_cap) {
		r = &row_mem[(row_first + row_count) % row_cap];
		row_count++;
	} else {
		row_first = (row_first + 1) % row_cap;
		r = &row_mem[(row_first + row_count - 1) % row_cap];
	}
	row_blank(r);
}

/* Append a row at the bottom and scroll the screen one line.  When the log is
 * not full yet the existing rows keep their place, so the view offset grows
 * with the origin to leave what the user is reading on screen. */
static void append_blank_row(void)
{
	int was_full = (row_count >= row_cap);

	row_add_blank();
	mark_full();   /* the scrollbar thumb may change size */
	if (was_full) {
		/* Every logical row moved down one slot; keep the input-line
		 * anchor pointing at the same row. */
		if (line_abs > 0)
			line_abs--;
	} else if (view) {
		view++;
	}
}

static void scroll_by(int delta)
{
	unsigned int maxs = row_count - rows;
	unsigned int nv = view;

	if (delta > 0) {
		nv += (unsigned int)delta;
	} else {
		unsigned int d = (unsigned int)(-delta);

		nv = (nv > d) ? nv - d : 0;
	}
	if (nv > maxs)
		nv = maxs;
	if (nv != view) {
		view = nv;
		mark_full();
	}
}

/* ---- Text output ---- */

static void cell_put(unsigned int x, unsigned int y, char ch)
{
	struct trow *r;

	if (x >= MAX_COLS || y >= rows)
		return;
	r = row_at(origin_row() + y);
	r->cells[x] = (unsigned short)(0x0A00 | (unsigned char)ch);
	if (x + 1 > r->len)
		r->len = x + 1;
	mark_row(y);
}

static void term_scroll(void)
{
	append_blank_row();
	cy = rows - 1;
}

static void term_newline(void)
{
	unsigned int old = cy;

	row_at(origin_row() + cy)->wrapped = 0;
	wrap_pending = 0;
	cx = 0;
	cy++;
	if (cy >= rows) {
		term_scroll();
		mark_full();
	} else {
		mark_row(old);
		mark_row(cy);
	}
}

/* The next glyph does not fit: the row continues on the next one, and the
 * two rows form one logical line that reflow must re-wrap as a unit. */
static void term_wrap(void)
{
	unsigned int old = cy;

	row_at(origin_row() + cy)->wrapped = 1;
	wrap_pending = 0;
	cx = 0;
	cy++;
	if (cy >= rows) {
		term_scroll();
		mark_full();
	} else {
		mark_row(old);
		mark_row(cy);
	}
}

static void term_putc(char c)
{
	if (c == '\n') {
		term_newline();
		return;
	}
	if (c == '\r') {
		wrap_pending = 0;
		cx = 0;
		mark_row(cy);
		return;
	}
	if (c == '\b') {
		wrap_pending = 0;
		if (cx > 0)
			cx--;
		cell_put(cx, cy, ' ');
		return;
	}
	if (c == '\t') {
		unsigned int n = 8 - (cx % 8);

		while (n--)
			term_putc(' ');
		return;
	}
	if (c < 0x20 || c > 0x7E)
		return;
	if (wrap_pending)
		term_wrap();
	cell_put(cx, cy, c);
	cx++;
	if (cx >= cols)
		wrap_pending = 1;   /* stay on this row until the next glyph */
}

static void term_puts(const char *s)
{
	while (*s)
		term_putc(*s++);
}

/* ESC[2J: erase the visible screen in place.  The rows stay in the log, so a
 * full-screen repaint (top) does not push a frame into the scrollback every
 * second, and anything above the screen is still there to scroll back to. */
static void term_erase_screen(void)
{
	unsigned int i;

	for (i = 0; i < rows; i++)
		row_blank(row_at(origin_row() + i));
	cx = cy = 0;
	wrap_pending = 0;
	mark_full();
}

/* The `clear` builtin: drop the screen and the whole scrollback. */
static void term_reset(void)
{
	row_first = 0;
	row_count = 0;
	view = 0;
	while (row_count < rows)
		row_add_blank();
	cx = cy = 0;
	wrap_pending = 0;
	mark_full();
}

static void cursor_set(unsigned int row, unsigned int col)
{
	unsigned int old = cy;

	if (row >= rows)
		row = rows - 1;
	if (col >= cols)
		col = cols - 1;
	cy = row;
	cx = col;
	wrap_pending = 0;
	mark_row(old);
	mark_row(cy);
}

/* Parse the exact "ESC[row;colH" cursor sequence used by io_set_cursor. */
static int esc_cursor(const char *p, unsigned int n)
{
	unsigned int row = 0, col = 0, i = 0;

	if (n < 5 || p[0] != 0x1B || p[1] != '[')
		return 0;
	i = 2;   /* skip "ESC [" */
	while (i < n && p[i] >= '0' && p[i] <= '9')
		row = row * 10 + (unsigned int)(p[i++] - '0');
	if (i >= n || p[i++] != ';')
		return 0;
	while (i < n && p[i] >= '0' && p[i] <= '9')
		col = col * 10 + (unsigned int)(p[i++] - '0');
	if (i >= n || p[i] != 'H')
		return 0;
	if (row < 1 || col < 1)
		return 0;
	cursor_set(row - 1, col - 1);
	return 1;
}

static void tty_feed(const char *buf, unsigned int n)
{
	unsigned int i = 0;

	while (i < n) {
		if (buf[i] == 0x1B) {
			unsigned int j = i + 1;
			char seq[24];
			unsigned int sn = 0;

			while (j < n && sn + 1 < sizeof(seq)) {
				seq[sn++] = buf[j];
				if ((buf[j] >= 'A' && buf[j] <= 'Z') ||
				    (buf[j] >= 'a' && buf[j] <= 'z'))
					break;
				j++;
			}
			seq[sn] = '\0';
			/* Rebuild with ESC prefix for the helper. */
			{
				char full[28];
				unsigned int m;

				full[0] = 0x1B;
				for (m = 0; m < sn && m + 1 < sizeof(full); m++)
					full[m + 1] = seq[m];
				full[sn + 1] = '\0';
				if (esc_cursor(full, sn + 1)) {
					i = j + 1;
					continue;
				}
			}
			if (sn > 1 && seq[sn - 1] == 'J' && seq[1] == '2') {
				term_erase_screen();
			} else if (sn && seq[sn - 1] == 'H') {
				cursor_set(0, 0);
			}
			i = j < n ? j + 1 : n;
			continue;
		}
		term_putc(buf[i++]);
	}
}

static void drain_tty(void)
{
	char buf[BUF_MAX];
	int n;

	while ((n = u_tty_read(tty, buf, sizeof(buf))) > 0)
		tty_feed(buf, (unsigned int)n);
}

/* ---- Scrollbar ---- */

static void sb_metrics(unsigned int *track_h, unsigned int *thumb_h,
		       unsigned int *thumb_y)
{
	unsigned int total = row_count ? row_count : 1;
	unsigned int t = (wi.height > SB_BOTTOM) ? wi.height - SB_BOTTOM
						 : wi.height;
	unsigned int maxs = row_count - rows;
	unsigned int h = (rows * t) / total;

	if (h < 12)
		h = 12;
	if (h > t)
		h = t;
	if (maxs == 0)
		*thumb_y = t - h;
	else
		*thumb_y = (t - h) * (maxs - view) / maxs;
	*track_h = t;
	*thumb_h = h;
}

static void draw_scrollbar(void)
{
	unsigned int track_h, thumb_h, thumb_y, color;
	unsigned int x = wi.width - SB_W;

	sb_metrics(&track_h, &thumb_h, &thumb_y);
	usurf_fill(&surf, (int)x, 0, SB_W, track_h, 0x1C1C24u);
	if (sb_drag)
		color = 0xD0D0E0u;
	else if (view)
		color = 0xB0A050u;
	else
		color = 0x808090u;
	usurf_fill(&surf, (int)x, (int)thumb_y, SB_W, thumb_h, color);
}

static void sb_set_from_y(int y)
{
	unsigned int track_h, thumb_h, thumb_y;
	unsigned int maxs = row_count - rows;
	unsigned int nv;
	int ty = y - sb_grab_ofs;

	sb_metrics(&track_h, &thumb_h, &thumb_y);
	(void)thumb_y;
	if (ty < 0)
		ty = 0;
	if (maxs == 0 || track_h <= thumb_h) {
		nv = 0;
	} else {
		if ((unsigned int)ty > track_h - thumb_h)
			ty = (int)(track_h - thumb_h);
		nv = maxs - (unsigned int)ty * maxs / (track_h - thumb_h);
	}
	if (nv != view) {
		view = nv;
		mark_full();
	}
}

static void handle_pointer(void)
{
	struct win_event e;
	struct win_pointer p;

	while (u_win_event(&e) == 1) {
		if (e.type == WIN_EV_POINTER_DOWN) {
			if (e.x >= (int)(wi.width - SB_W) && e.y >= 0 &&
			    e.y < (int)wi.height) {
				unsigned int track_h, thumb_h, thumb_y;

				sb_metrics(&track_h, &thumb_h, &thumb_y);
				(void)track_h;
				if ((unsigned int)e.y >= thumb_y &&
				    (unsigned int)e.y < thumb_y + thumb_h) {
					sb_drag = 1;
					sb_grab_ofs = e.y - (int)thumb_y;
				} else if ((unsigned int)e.y < thumb_y) {
					scroll_by((int)rows - 1);
				} else {
					scroll_by(-((int)rows - 1));
				}
			}
		} else if (e.type == WIN_EV_POINTER_UP) {
			if (sb_drag) {
				sb_drag = 0;
				mark_full();
			}
		}
	}
	if (sb_drag && u_win_pointer(&p) == 0) {
		if (!(p.buttons & 1)) {
			sb_drag = 0;
			mark_full();
		} else {
			sb_set_from_y(p.y);
		}
	}
}

/* ---- Rendering ---- */

static void render(void)
{
	unsigned int x, y, y0, y1, maxs, start;

	if (!dirty)
		return;
	maxs = row_count - rows;
	if (view > maxs) {
		view = maxs;
		dirty_full = 1;
	}
	start = origin_row() - view;
	if (dirty_full) {
		y0 = 0;
		y1 = rows - 1;
	} else if (dmg_valid) {
		y0 = dmg_y0;
		y1 = dmg_y1;
	} else {
		dirty = 0;
		return;
	}

	for (y = y0; y <= y1; y++) {
		unsigned int idx = start + y;
		unsigned short *dst = &view_cells[y * cols];

		if (idx < row_count) {
			struct trow *r = row_at(idx);
			unsigned int n = r->len;

			if (n > cols)
				n = cols;
			for (x = 0; x < n; x++)
				dst[x] = r->cells[x];
			for (; x < cols; x++)
				dst[x] = 0x0A20;
		} else {
			for (x = 0; x < cols; x++)
				dst[x] = 0x0A20;
		}
	}

	if (dirty_full)
		usurf_fill(&surf, 0, 0, wi.width, wi.height, 0x000000u);
	else
		usurf_fill(&surf, 0, (int)(y0 * 16), wi.width,
			   (y1 - y0 + 1) * 16, 0x000000u);
	usurf_cells(&surf, &view_cells[y0 * cols], cols, y1 - y0 + 1,
		    0, (int)(y0 * 16), 0, 0, wi.width, wi.height);
	if (view == 0 && cursor_on && cy >= y0 && cy <= y1) {
		unsigned int ccx = (cx < cols) ? cx : (cols ? cols - 1 : 0);

		usurf_fill(&surf, (int)ccx * 8, (int)cy * 16, 2, 16,
			   0xFFFFFFu);
	}
	draw_scrollbar();
	if (dirty_full)
		u_win_present();
	else
		u_win_present_rect(0, (int)(y0 * 16), wi.width,
				   (y1 - y0 + 1) * 16);
	dirty = 0;
	dirty_full = 0;
	dmg_valid = 0;
}

static void line_show(void)
{
	unsigned int i, x, base, k;
	struct trow *r;

	mark_row(cy);
	if (view) {
		view = 0;
		mark_full();
	}
	base = line_abs;
	if (base < origin_row())
		base = origin_row();
	if (base >= row_count)
		base = row_count - 1;
	line_abs = base;
	r = row_at(base);
	if (r->len > line_col)
		r->len = line_col;
	r->wrapped = 0;
	/* The input line is the tail of the log: rows after its start are
	 * leftovers of the previous, possibly longer, version of the line. */
	for (k = 1; base + k < row_count; k++) {
		row_blank(row_at(base + k));
		if (base + k >= origin_row())
			mark_row(base + k - origin_row());
	}

	cx = line_col;
	cy = base - origin_row();
	wrap_pending = 0;
	if (cx >= cols)
		term_wrap();   /* the prompt filled the row exactly */
	for (i = 0; i < line_len; i++)
		term_putc(line[i]);
	for (x = cx; x < cols; x++)
		cell_put(x, cy, ' ');
	mark_row(cy);
}

/* ---- Reflow ---- */

/* Absolute row index of `idx` and the character offset of (idx, col) in the
 * logical line that contains it. */
static void pos_capture(struct pos_map *m, unsigned int idx, unsigned int col)
{
	unsigned int ls, off = 0, j;

	if (idx >= row_count)
		idx = row_count - 1;
	if (col > MAX_COLS)
		col = MAX_COLS;
	ls = idx;
	while (ls > 0 && row_at(ls - 1)->wrapped)
		ls--;
	for (j = ls; j < idx; j++) {
		unsigned int l = row_at(j)->len;

		if (l > MAX_COLS)
			l = MAX_COLS;
		off += l;
	}
	{
		unsigned int l = row_at(idx)->len;

		if (col > l)
			col = l;
		off += col;
	}
	m->abs = idx;
	m->col = col;
	m->start = ls;
	m->off = off;
	m->dst = POS_NONE;
}

/* Re-wrap every logical line at `new_cols` into the scratch ring, then copy
 * the retained tail back.  The two maps are moved through the same rebuild so
 * the cursor and the shell's input line keep pointing at the same text. */
static void reflow(unsigned int new_cols, struct pos_map *cur,
		   struct pos_map *lin)
{
	unsigned int i = 0, out_total = 0, keep, drop, k;

	while (i < row_count) {
		unsigned int ls = i, j = i, len = 0;
		unsigned int out_start = out_total;

		while (1) {
			struct trow *r = row_at(j);
			unsigned int l = r->len;

			if (l > MAX_COLS)
				l = MAX_COLS;
			len += l;
			j++;
			if (!r->wrapped || j >= row_count)
				break;
		}
		if (ls == cur->start)
			cur->dst = out_start;
		if (ls == lin->start)
			lin->dst = out_start;

		if (len == 0) {
			row_blank(dst_row(out_total++));
		} else {
			unsigned int p = 0, srow = ls, sc = 0;

			while (p < len) {
				struct trow *d = dst_row(out_total++);
				unsigned int n = len - p;
				unsigned int q = 0;

				if (n > new_cols)
					n = new_cols;
				row_blank(d);
				while (q < n) {
					struct trow *s = row_at(srow);
					unsigned int sl = s->len;

					if (sl > MAX_COLS)
						sl = MAX_COLS;
					if (sc >= sl) {
						srow++;
						sc = 0;
						continue;
					}
					d->cells[q++] = s->cells[sc++];
					p++;
				}
				d->len = n;
				d->wrapped = (p < len);
			}
		}
		i = j;
	}

	keep = (out_total < row_cap) ? out_total : row_cap;
	drop = out_total - keep;
	for (k = 0; k < keep; k++) {
		struct trow *s = dst_row(drop + k);
		struct trow *d = &row_mem[k];
		unsigned int c;

		d->len = s->len;
		d->wrapped = s->wrapped;
		for (c = 0; c < MAX_COLS; c++)
			d->cells[c] = s->cells[c];
	}
	row_first = 0;
	row_count = keep;

	if (cur->dst != POS_NONE) {
		unsigned int a = cur->dst + cur->off / new_cols;

		cur->abs = (a >= drop) ? a - drop : 0;
		cur->col = cur->off % new_cols;
	}
	if (lin->dst != POS_NONE) {
		unsigned int a = lin->dst + lin->off / new_cols;

		lin->abs = (a >= drop) ? a - drop : 0;
		lin->col = lin->off % new_cols;
	}
}

static void finish_resize(unsigned int new_rows, struct pos_map *cur,
			  struct pos_map *lin)
{
	unsigned int origin;

	/* Blank rows below the last written one are only padding.  Dropping
	 * them keeps the cursor on screen when the window gets shorter;
	 * otherwise the tail-anchored screen would scroll the prompt away. */
	while (row_count > 0) {
		struct trow *r = row_at(row_count - 1);

		if (r->len != 0 || r->wrapped)
			break;
		row_count--;
	}
	rows = new_rows;
	while (row_count < rows)
		row_add_blank();
	origin = row_count - rows;
	if (view > origin)
		view = origin;

	if (cur->abs < origin)
		cur->abs = origin;
	if (cur->abs >= row_count) {
		row_add_blank();
		cur->abs = row_count - 1;
	}
	origin = row_count - rows;
	cy = cur->abs - origin;
	cx = (cur->col < cols) ? cur->col : cols - 1;

	if (lin->abs < origin)
		lin->abs = origin;
	if (lin->abs >= row_count)
		lin->abs = row_count - 1;
	line_abs = lin->abs;
	line_col = (lin->col < cols) ? lin->col : cols - 1;
	mark_full();
}

static void handle_resize(void)
{
	struct win_info cur;
	struct pos_map cpos, lpos;
	unsigned int new_cols, new_rows;

	if (u_win_info(&cur) != 0)
		return;
	if (cur.width == wi.width && cur.height == wi.height)
		return;
	if (u_win_ack_resize() != 0)
		return;

	new_cols = (cur.width > SB_W) ? (cur.width - SB_W) / 8 : 1;
	if (new_cols < 1)
		new_cols = 1;
	if (new_cols > MAX_COLS)
		new_cols = MAX_COLS;
	new_rows = cur.height / 16;
	if (new_rows < 1)
		new_rows = 1;
	if (new_rows > MAX_ROWS)
		new_rows = MAX_ROWS;
	if (new_rows > row_cap)
		new_rows = row_cap;

	pos_capture(&cpos, origin_row() + cy, cx);
	pos_capture(&lpos, line_abs, line_col);

	wi = cur;
	usurf_init(&surf, wi.fb, wi.width, wi.height, wi.width * 2, 16);

	if (new_cols != cols) {
		reflow(new_cols, &cpos, &lpos);
		cols = new_cols;
	}
	finish_resize(new_rows, &cpos, &lpos);
}

static void line_set(const char *s)
{
	unsigned int i = 0;

	while (s[i] && i + 1 < LINE_MAX) {
		line[i] = s[i];
		i++;
	}
	line[i] = '\0';
	line_len = i;
	line_show();
}

static void history_load(int n)
{
	unsigned int idx;

	if (n <= 0 || n > (int)hist_count)
		return;
	idx = (hist_next + HIST_MAX - (unsigned int)n) % HIST_MAX;
	line_set(hist[idx]);
}

static void prompt(void)
{
	term_putc('[');
	term_puts(cwd);
	term_puts("] > ");
	line_len = 0;
	line[0] = '\0';
	line_abs = origin_row() + cy;
	line_col = cx;
	hist_browse = 0;
	mark_row(cy);
}

static void print_num(unsigned int n)
{
	char b[12];
	unsigned int i = 0;

	if (!n) {
		term_putc('0');
		return;
	}
	while (n) {
		b[i++] = (char)('0' + n % 10);
		n /= 10;
	}
	while (i)
		term_putc(b[--i]);
}

static unsigned int parse_num(const char *s)
{
	unsigned int n = 0;

	while (*s == ' ')
		s++;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (unsigned int)(*s++ - '0');
	return n;
}

static void split_line(char *word, unsigned int wsz, char *args,
		       unsigned int asz)
{
	unsigned int i = 0, n = 0;

	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && n + 1 < wsz)
		word[n++] = line[i++];
	word[n] = '\0';
	while (line[i] == ' ')
		i++;
	n = 0;
	while (line[i] && n + 1 < asz)
		args[n++] = line[i++];
	while (n && args[n - 1] == ' ')
		n--;
	args[n] = '\0';
}

static int make_path(const char *dir, const char *word, char *out,
		     unsigned int max)
{
	unsigned int i = 0, j;

	if (word[0] == '/') {
		for (j = 0; word[j] && j + 1 < max; j++)
			out[j] = word[j];
		out[j] = '\0';
		return 0;
	}
	while (dir[i] && i + 1 < max)
		out[i] = dir[i], i++;
	if (i && out[i - 1] != '/' && i + 1 < max)
		out[i++] = '/';
	for (j = 0; word[j] && i + 1 < max; j++)
		out[i++] = word[j];
	out[i] = '\0';
	return 0;
}

static int try_spawn(const char *word, const char *args, char *used_path,
		     unsigned int used_size)
{
	char p[VFS_PATH_MAX];
	int pid;

	if (word[0] == '/') {
		for (unsigned int i = 0; word[i] && i + 1 < used_size; i++)
			used_path[i] = word[i];
		return u_spawn(word, args, tty, cwd);
	}
	/* Try each PATH component. */
	{
		unsigned int i = 0, start = 0;

		while (1) {
			unsigned int n = 0;
			char dir[VFS_PATH_MAX];

			while (path[i] && path[i] != ':') {
				if (n + 1 < sizeof(dir))
					dir[n++] = path[i];
				i++;
			}
			dir[n] = '\0';
			if (dir[0]) {
				make_path(dir, word, p, sizeof(p));
				pid = u_spawn(p, args, tty, cwd);
				if (pid >= 0) {
					for (n = 0; p[n] && n + 1 < used_size; n++)
						used_path[n] = p[n];
					used_path[n] = '\0';
					return pid;
				}
				if (pid == -2)
					return -2;
			}
			if (!path[i])
				break;
			i++;
			start = i;
			(void)start;
		}
	}
	return -1;
}

static void run_line(void)
{
	char word[LINE_MAX], args[LINE_MAX];
	char used[VFS_PATH_MAX];
	int pid;

	split_line(word, sizeof(word), args, sizeof(args));
	if (!word[0]) {
		prompt();
		return;
	}
	if (word[0] == 'c' && word[1] == 'd' && !word[2]) {
		if (u_chdir(args[0] ? args : "/") != 0)
			term_puts("cd: no such directory\n");
		u_getcwd(cwd, sizeof(cwd));
		prompt();
		return;
	}
	if (word[0] == 'p' && word[1] == 'a' && word[2] == 't' && !word[3]) {
		unsigned int i;

		if (!args[0]) {
			term_puts(path);
			term_putc('\n');
		} else {
			for (i = 0; args[i] && i + 1 < sizeof(path); i++)
				path[i] = args[i];
			path[i] = '\0';
		}
		prompt();
		return;
	}
	if (word[0] == 'h' && word[1] == 'e' && word[2] == 'l' &&
	    word[3] == 'p' && !word[4]) {
		pid = u_spawn_kcmd("help", "", tty);
		if (pid > 0) {
			fg_pid = pid;
			return;
		}
		term_puts("builtins: cd path help clear ps kill\n");
		term_puts("commands: files in ");
		term_puts(path);
		term_putc('\n');
		prompt();
		return;
	}
	if (word[0] == 'c' && word[1] == 'l' && word[2] == 'e' &&
	    word[3] == 'a' && word[4] == 'r' && !word[5]) {
		term_reset();
		prompt();
		return;
	}
	if (word[0] == 'p' && word[1] == 's' && !word[2]) {
		struct sys_procinfo pi;
		unsigned int i = 0;

		term_puts("PID  STATE  W CPU NAME\n");
		while (u_syscall2(SYS_PROCINFO, (int)i, (int)&pi) == 1) {
			print_num(pi.pid);
			term_puts("  ");
			if (pi.state == 1) term_puts("ready");
			else if (pi.state == 2) term_puts("run  ");
			else if (pi.state == 4) term_puts("sleep");
			else if (pi.state == 5) term_puts("block");
			else term_puts("?    ");
			term_puts("  ");
			print_num(pi.weight);
			term_puts("   ");
			if (pi.cpu != 0xFFFFFFFFu)
				print_num(pi.cpu);
			else
				term_putc('-');
			term_puts("   ");
			term_puts(pi.name);
			term_putc('\n');
			i++;
		}
		prompt();
		return;
	}
	if (word[0] == 'k' && word[1] == 'i' && word[2] == 'l' &&
	    word[3] == 'l' && !word[4]) {
		unsigned int pid = parse_num(args);

		if (!pid || u_kill(pid) != 0)
			term_puts("kill: no such process\n");
		prompt();
		return;
	}

	pid = try_spawn(word, args, used, sizeof(used));
	if (pid == -2)
		term_puts("not an executable\n");
	else if (pid < 0) {
		pid = u_spawn_kcmd(word, args, tty);
		if (pid < 0)
			term_puts("command not found\n");
	}
	if (pid > 0)
		fg_pid = pid;
	else
		prompt();
}

static void submit_line(void)
{
	term_putc('\n');
	if (line_len) {
		unsigned int i;

		for (i = 0; line[i] && i < sizeof(hist[hist_next]) - 1; i++)
			hist[hist_next][i] = line[i];
		hist[hist_next][i] = '\0';
		hist_next = (hist_next + 1) % HIST_MAX;
		if (hist_count < HIST_MAX)
			hist_count++;
	}
	hist_browse = 0;
	run_line();
}

static unsigned int name_len(const char *s, unsigned int max)
{
	unsigned int n = 0;

	while (n < max && s[n])
		n++;
	while (n && s[n - 1] == ' ')
		n--;
	return n;
}

static int prefix_is(const char *word, unsigned int wl, const char *name,
		     unsigned int nl)
{
	unsigned int i;

	if (wl > nl)
		return 0;
	for (i = 0; i < wl; i++)
		if (word[i] != name[i])
			return 0;
	return 1;
}

/* Complete the token at the end of the line.  A token after the command word
 * (or any token containing '/') is a path; the first token is a command. */
static void complete_line(void)
{
	unsigned int start = line_len;
	unsigned int tlen, slash = 0, i, n, bl, cl, matches = 0;
	char token[LINE_MAX], dir[VFS_PATH_MAX], base[LINE_MAX];
	char cand[LINE_MAX];
	int has_slash = 0, cand_dir = 0;

	while (start > 0 && line[start - 1] != ' ')
		start--;
	tlen = line_len - start;
	if (!tlen)
		return;
	for (i = 0; i < tlen && i + 1 < sizeof(token); i++)
		token[i] = line[start + i];
	token[i] = '\0';
	for (i = 0; i < tlen; i++)
		if (token[i] == '/') {
			has_slash = 1;
			slash = i;
		}

	if (has_slash || start > 0) {
		/* Path token: split into directory + basename prefix. */
		if (!has_slash) {
			dir[0] = '.';
			dir[1] = '\0';
			for (i = 0; i < tlen && i + 1 < sizeof(base); i++)
				base[i] = token[i];
			base[i] = '\0';
			bl = i;
		} else if (slash == 0) {
			dir[0] = '/';
			dir[1] = '\0';
			for (i = 0; i + 1 < tlen && i + 1 < sizeof(base); i++)
				base[i] = token[i + 1];
			base[i] = '\0';
			bl = i;
		} else {
			if (slash >= sizeof(dir))
				return;
			for (i = 0; i < slash; i++)
				dir[i] = token[i];
			dir[i] = '\0';
			for (i = 0; slash + 1 + i < tlen &&
				    i + 1 < sizeof(base); i++)
				base[i] = token[slash + 1 + i];
			base[i] = '\0';
			bl = i;
		}

		if (dir[0] == '/' && dir[1] == '\0') {
			struct sys_mount m;

			for (i = 0;; i++) {
				if (u_syscall2(SYS_LIST_MOUNTS, (int)i,
					       (int)&m) != 1)
					break;
				n = name_len(m.name, sizeof(m.name));
				if (!prefix_is(base, bl, m.name, n))
					continue;
				matches++;
				if (matches == 1) {
					cl = 0;
					cand[cl++] = '/';
					for (n = 0; m.name[n] && cl + 1 < sizeof(cand); n++)
						cand[cl++] = m.name[n];
					cand[cl] = '\0';
					cand_dir = 1;
				}
			}
		} else {
			struct sys_dirent d;

			for (i = 0;; i++) {
				char disp[64];
				unsigned int dl;

				if (u_syscall3(SYS_LIST_DIR, (int)dir, (int)i,
					       (int)&d) != 1)
					break;
				if (d.raw_83)
					u_fat_name(d.name, disp, sizeof(disp));
				else {
					for (dl = 0; dl + 1 < sizeof(disp) &&
						      d.name[dl]; dl++)
						disp[dl] = d.name[dl];
					disp[dl] = '\0';
				}
				n = name_len(disp, sizeof(disp));
				if (n == 0 ||
				    (n == 1 && disp[0] == '.') ||
				    (n == 2 && disp[0] == '.' && disp[1] == '.'))
					continue;
				if (!prefix_is(base, bl, disp, n))
					continue;
				matches++;
				if (matches == 1) {
					cl = 0;
					if (slash > 0) {
						unsigned int k;

						for (k = 0; k < slash &&
							    cl + 1 < sizeof(cand); k++)
							cand[cl++] = token[k];
						if (cl && cand[cl - 1] != '/' &&
						    cl + 1 < sizeof(cand))
							cand[cl++] = '/';
					}
					for (n = 0; disp[n] &&
						    n < sizeof(disp) &&
						    cl + 1 < sizeof(cand); n++)
						cand[cl++] = disp[n];
					cand[cl] = '\0';
					cand_dir = d.is_dir ? 1 : 0;
				}
			}
		}
	} else {
		/* Command-name completion: builtins + /ramfs/bin. */
		static const char * const builtins[] = {
			"cd", "path", "help", "clear", "bootlog", "ps", "kill"
		};
		struct sys_dirent d;

		for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
			n = name_len(builtins[i], 16);
			if (!prefix_is(token, tlen, builtins[i], n))
				continue;
			matches++;
			if (matches == 1) {
				for (n = 0; builtins[i][n] && n + 1 < sizeof(cand); n++)
					cand[n] = builtins[i][n];
				cand[n] = '\0';
			}
		}
		for (i = 0;; i++) {
			if (u_syscall3(SYS_LIST_DIR, (int)"/ramfs/bin", (int)i,
				       (int)&d) != 1)
				break;
			n = name_len(d.name, sizeof(d.name));
			if (!prefix_is(token, tlen, d.name, n))
				continue;
			matches++;
			if (matches == 1) {
				for (n = 0; d.name[n] && n < sizeof(d.name) &&
					    n + 1 < sizeof(cand); n++)
					cand[n] = d.name[n];
				cand[n] = '\0';
			}
		}
		cand_dir = 0;
	}

	if (matches != 1)
		return;

	line_len = start;
	for (i = 0; cand[i] && line_len + 1 < LINE_MAX; i++)
		line[line_len++] = cand[i];
	if (cand_dir && line_len + 1 < LINE_MAX)
		line[line_len++] = '/';
	else if (line_len + 1 < LINE_MAX)
		line[line_len++] = ' ';
	line[line_len] = '\0';
	line_show();
}

static int is_page_key(int k)
{
	return k == IO_KEY_PAGE_UP || k == IO_KEY_PAGE_DOWN ||
	       k == IO_KEY_HOME || k == IO_KEY_END;
}

static void handle_key(int k)
{
	if (k == IO_KEY_PAGE_UP) {
		scroll_by((int)rows - 1);
		return;
	}
	if (k == IO_KEY_PAGE_DOWN) {
		scroll_by(-((int)rows - 1));
		return;
	}
	if (k == IO_KEY_HOME) {
		scroll_by((int)row_count);
		return;
	}
	if (k == IO_KEY_END) {
		scroll_by(-(int)row_count);
		return;
	}
	if (view) {
		/* Typing always means the prompt: jump back to the tail. */
		view = 0;
		mark_full();
	}
	if (k == 0x03) {
		if (fg_pid > 0) {
			u_kill((unsigned int)fg_pid);
			fg_pid = -1;
			term_puts("^C\n");
			prompt();
		} else {
			line_len = 0;
			line[0] = '\0';
			term_puts("^C\n");
			prompt();
		}
		return;
	}
	if (k == '\n' || k == '\r') {
		submit_line();
		return;
	}
	if (k == '\b') {
		if (line_len) {
			line_len--;
			line[line_len] = '\0';
			line_show();
		}
		return;
	}
	if (k == IO_KEY_UP) {
		if ((int)hist_browse < (int)hist_count) {
			hist_browse++;
			history_load(hist_browse);
		}
		return;
	}
	if (k == IO_KEY_DOWN) {
		if (hist_browse > 0) {
			hist_browse--;
			if (hist_browse == 0)
				line_set("");
			else
				history_load(hist_browse);
		}
		return;
	}
	if (k == '\t') {
		complete_line();
		return;
	}
	if (k < 0x20 || k > 0x7E)
		return;
	if (line_len + 1 < LINE_MAX) {
		hist_browse = 0;
		line[line_len++] = (char)k;
		line[line_len] = '\0';
		term_putc((char)k);
	}
}

static void term_alloc_rows(void)
{
	unsigned int bytes;

	row_cap = SCROLL_ROWS_PREF;
	bytes = 2 * row_cap * (unsigned int)sizeof(struct trow);
	row_mem = (struct trow *)u_mem_alloc(bytes);
	if (!row_mem) {
		row_cap = SCROLL_ROWS_MIN;
		bytes = 2 * row_cap * (unsigned int)sizeof(struct trow);
		row_mem = (struct trow *)u_mem_alloc(bytes);
	}
	if (row_mem) {
		row_scratch = row_mem + row_cap;
	} else {
		row_mem = base_rows;
		row_scratch = base_scratch;
		row_cap = MAX_ROWS;
	}
}

void _start(const char *arg)
{
	unsigned int now;

	(void)arg;
	tty = u_tty_create();
	if (tty <= 0)
		u_exit(1);
	if (u_win_create(512, 512, "term", &wi) != 0)
		u_exit(1);
	usurf_init(&surf, wi.fb, wi.width, wi.height, wi.width * 2, 16);
	term_alloc_rows();
	cols = (wi.width > SB_W) ? (wi.width - SB_W) / 8 : 1;
	if (cols < 1)
		cols = 1;
	if (cols > MAX_COLS)
		cols = MAX_COLS;
	rows = wi.height / 16;
	if (rows < 1)
		rows = 1;
	if (rows > MAX_ROWS)
		rows = MAX_ROWS;
	if (rows > row_cap)
		rows = row_cap;
	term_reset();
	u_getcwd(cwd, sizeof(cwd));
	{
		const char *def = "/ramfs/bin";
		unsigned int i;

		for (i = 0; def[i] && i + 1 < sizeof(path); i++)
			path[i] = def[i];
		path[i] = '\0';
	}
	prompt();
	render();

	for (;;) {
		int got = 0;

		handle_resize();
		drain_tty();
		if (fg_pid > 0) {
			if (!u_alive((unsigned int)fg_pid)) {
				fg_pid = -1;
				drain_tty();
				prompt();
			} else {
				int k = u_key_poll();

				while (k) {
					if (k == 0x03 || is_page_key(k))
						handle_key(k);
					k = u_key_poll();
				}
			}
		} else {
			int k = u_win_key_wait(sb_drag ? 10 : 100);

			if (k < 0)
				u_exit(1);
			while (k) {
				handle_key(k);
				got = 1;
				k = u_key_poll();
			}
		}
		handle_pointer();
		now = u_ticks_ms();
		if (now - last_blink >= 500) {
			last_blink = now;
			cursor_on = !cursor_on;
			if (view == 0)
				mark_row(cy);
		}
		render();
		u_sleep_ms((got || sb_drag) ? 1 : 10);
	}
}
