/* mines: a small Minesweeper for a canvas window.
 *
 * The board is 9x9 with 10 mines.  The first reveal is always safe: mines are
 * placed after it, outside the clicked cell and its eight neighbours.  Left
 * click reveals, right click toggles a flag, R starts a new game and ESC
 * closes the window.  The window can be resized; the board is re-laid out.
 */

#include "ucmd_lib.h"
#include "ngui.h"

#define MW              9
#define MH              9
#define MINE_COUNT      10
#define TOTAL_CELLS     (MW * MH)

#define START_W         300
#define START_H         360
#define PAD             10
#define HEADER_H        42
#define FOOTER_H        20

#define C_BG            NGUI_RGB(24, 28, 40)
#define C_PANEL         NGUI_RGB(38, 43, 58)
#define C_TILE          NGUI_RGB(62, 72, 96)
#define C_TILE_HOT      NGUI_RGB(78, 90, 120)
#define C_TILE_HI       NGUI_RGB(112, 126, 160)
#define C_TILE_LO       NGUI_RGB(28, 32, 44)
#define C_OPEN          NGUI_RGB(42, 48, 62)
#define C_GRID          NGUI_RGB(30, 35, 46)
#define C_BORDER        NGUI_RGB(92, 99, 124)
#define C_DIM           NGUI_RGB(145, 151, 170)
#define C_FLAG          NGUI_RGB(238, 116, 116)
#define C_MINE          NGUI_RGB(255, 110, 110)
#define C_EXPLODED      NGUI_RGB(130, 36, 44)
#define C_FACE          NGUI_RGB(238, 198, 88)
#define C_FACE_DK       NGUI_RGB(125, 88, 20)
#define C_WIN           NGUI_RGB(120, 220, 140)
#define C_LOSE          NGUI_RGB(238, 116, 116)

struct mines_layout {
	int cell;
	int bx, by, bw, bh;
	int face_x, face_y, face_w, face_h;
	int counter_x, counter_y;
	int timer_x, timer_y;
	int footer_y;
};

static unsigned char mine[TOTAL_CELLS];
static unsigned char opened[TOTAL_CELLS];
static unsigned char flagged[TOTAL_CELLS];
static unsigned char adj[TOTAL_CELLS];

static int started;
static int over;
static int won;
static int exploded = -1;
static int opened_count;
static int flag_count;
static int last_second;
static int quit;

static unsigned int start_ms;
static unsigned int end_ms;
static unsigned int rng_state = 0x6D2B79F5u;
static unsigned int game_seq;
static unsigned int prev_right;

static struct ngui_rect rect_make(int x, int y, int w, int h)
{
	struct ngui_rect r;

	r.x = x;
	r.y = y;
	r.w = w;
	r.h = h;
	return r;
}

static int point_in(const struct ngui_rect *r, int x, int y)
{
	return x >= r->x && y >= r->y &&
	       x < r->x + r->w && y < r->y + r->h;
}

static unsigned int rng_next(void)
{
	unsigned int x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x ? x : 0x6D2B79F5u;
	return rng_state;
}

static void reset_game(void)
{
	unsigned int i;

	for (i = 0; i < TOTAL_CELLS; i++) {
		mine[i] = 0;
		opened[i] = 0;
		flagged[i] = 0;
		adj[i] = 0;
	}
	started = 0;
	over = 0;
	won = 0;
	exploded = -1;
	opened_count = 0;
	flag_count = 0;
	last_second = 0;
	start_ms = 0;
	end_ms = 0;
	game_seq++;
	rng_state = (u_ticks_ms() * 2654435761u) ^
		    (game_seq * 2246822519u) ^ 0x9E3779B9u;
	if (!rng_state)
		rng_state = 0x6D2B79F5u;
}

static void compute_adj(void)
{
	int y, x, dy, dx;

	for (y = 0; y < MH; y++) {
		for (x = 0; x < MW; x++) {
			int n = 0;

			for (dy = -1; dy <= 1; dy++) {
				for (dx = -1; dx <= 1; dx++) {
					int nx = x + dx;
					int ny = y + dy;

					if ((!dx && !dy) || nx < 0 || nx >= MW ||
					    ny < 0 || ny >= MH)
						continue;
					if (mine[ny * MW + nx])
						n++;
				}
			}
			adj[y * MW + x] = (unsigned char)n;
		}
	}
}

static void place_mines(int safe)
{
	int sx = safe % MW;
	int sy = safe / MW;
	int placed = 0;
	unsigned int guard = 0;

	while (placed < MINE_COUNT && guard++ < 100000u) {
		int idx = (int)(rng_next() % TOTAL_CELLS);
		int x = idx % MW;
		int y = idx / MW;

		if (mine[idx])
			continue;
		if (x >= sx - 1 && x <= sx + 1 &&
		    y >= sy - 1 && y <= sy + 1)
			continue;
		mine[idx] = 1;
		placed++;
	}
	compute_adj();
}

static int reveal_cell(int idx)
{
	int queue[TOTAL_CELLS];
	int head = 0, tail = 0;

	if (idx < 0 || idx >= TOTAL_CELLS)
		return 0;
	if (over || opened[idx] || flagged[idx])
		return 0;
	if (!started) {
		place_mines(idx);
		started = 1;
		start_ms = u_ticks_ms();
		last_second = 0;
	}
	if (mine[idx]) {
		over = 1;
		exploded = idx;
		end_ms = u_ticks_ms();
		return 1;
	}

	opened[idx] = 1;
	opened_count++;
	queue[tail++] = idx;
	while (head < tail) {
		int cur = queue[head++];
		int y = cur / MW;
		int x = cur % MW;
		int dy, dx;

		if (adj[cur] != 0)
			continue;
		for (dy = -1; dy <= 1; dy++) {
			for (dx = -1; dx <= 1; dx++) {
				int nx = x + dx;
				int ny = y + dy;
				int n;

				if ((!dx && !dy) || nx < 0 || nx >= MW ||
				    ny < 0 || ny >= MH)
					continue;
				n = ny * MW + nx;
				if (opened[n] || flagged[n] || mine[n])
					continue;
				opened[n] = 1;
				opened_count++;
				queue[tail++] = n;
			}
		}
	}

	if (opened_count >= TOTAL_CELLS - MINE_COUNT) {
		int i;

		over = 1;
		won = 1;
		end_ms = u_ticks_ms();
		for (i = 0; i < TOTAL_CELLS; i++) {
			if (mine[i] && !flagged[i]) {
				flagged[i] = 1;
				flag_count++;
			}
		}
	}
	return 1;
}

static int toggle_flag(int idx)
{
	if (idx < 0 || idx >= TOTAL_CELLS)
		return 0;
	if (over || opened[idx])
		return 0;
	if (flagged[idx]) {
		flagged[idx] = 0;
		flag_count--;
	} else {
		flagged[idx] = 1;
		flag_count++;
	}
	return 1;
}

static int elapsed_seconds(void)
{
	unsigned int now, ms;

	if (!started)
		return 0;
	now = over ? end_ms : u_ticks_ms();
	ms = now - start_ms;
	if (ms / 1000 > 999u)
		return 999;
	return (int)(ms / 1000);
}

static void format3(int v, char *out)
{
	if (v < 0) {
		out[0] = '-';
		if (v < -99)
			v = -99;
		v = -v;
		out[1] = (char)('0' + v / 10);
		out[2] = (char)('0' + v % 10);
		out[3] = '\0';
		return;
	}
	if (v > 999)
		v = 999;
	out[0] = (char)('0' + v / 100);
	out[1] = (char)('0' + (v / 10) % 10);
	out[2] = (char)('0' + v % 10);
	out[3] = '\0';
}

static void compute_layout(struct ngui *ui, struct mines_layout *l)
{
	int region_h = ui->h - HEADER_H - FOOTER_H;
	int avail_w = ui->w - 2 * PAD;
	int avail_h = region_h - 2 * PAD;
	int cell;

	if (avail_w < 1)
		avail_w = 1;
	if (avail_h < 1)
		avail_h = 1;
	cell = avail_w / MW;
	if (cell > avail_h / MH)
		cell = avail_h / MH;
	if (cell < 10)
		cell = 10;
	if (cell > 40)
		cell = 40;

	l->cell = cell;
	l->bw = cell * MW;
	l->bh = cell * MH;
	l->bx = (ui->w - l->bw) / 2;
	if (l->bx < 0)
		l->bx = 0;
	l->by = HEADER_H + (region_h - l->bh) / 2;
	if (l->by < HEADER_H)
		l->by = HEADER_H;

	l->face_w = 28;
	l->face_h = 28;
	l->face_x = (ui->w - l->face_w) / 2;
	l->face_y = (HEADER_H - l->face_h) / 2;
	l->counter_x = PAD;
	l->counter_y = (HEADER_H - 16) / 2;
	l->timer_x = ui->w - PAD - 36;
	if (l->timer_x < l->counter_x + 44)
		l->timer_x = l->counter_x + 44;
	l->timer_y = l->counter_y;
	l->footer_y = ui->h - FOOTER_H + 2;
}

static struct ngui_rect face_rect(const struct mines_layout *l)
{
	return rect_make(l->face_x, l->face_y, l->face_w, l->face_h);
}

static int cell_at(const struct mines_layout *l, int px, int py)
{
	int cx, cy;

	if (l->cell <= 0 || px < l->bx || py < l->by)
		return -1;
	cx = (px - l->bx) / l->cell;
	cy = (py - l->by) / l->cell;
	if (cx < 0 || cx >= MW || cy < 0 || cy >= MH)
		return -1;
	return cy * MW + cx;
}

static void fill_circle(struct ngui *ui, int cx, int cy, int r,
			unsigned short color)
{
	int dy;

	for (dy = -r; dy <= r; dy++) {
		int dx = 0;

		while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
			dx++;
		if (dx > 0)
			ngui_fill_rect(ui,
				       rect_make(cx - dx, cy + dy,
						 dx * 2 + 1, 1), color);
	}
}

static void draw_mine_icon(struct ngui *ui, int cx, int cy, int r,
			   unsigned short color)
{
	int s = r + 2;

	ngui_line(ui, cx - s, cy, cx + s, cy, color);
	ngui_line(ui, cx, cy - s, cx, cy + s, color);
	ngui_line(ui, cx - s + 2, cy - s + 2, cx + s - 2, cy + s - 2, color);
	ngui_line(ui, cx - s + 2, cy + s - 2, cx + s - 2, cy - s + 2, color);
	fill_circle(ui, cx, cy, r, color);
}

static void draw_flag_icon(struct ngui *ui, struct ngui_rect r,
			   unsigned short color)
{
	int cx = r.x + r.w / 2;
	int top = r.y + r.h / 2 - 7;
	int pole_h = 13;
	int fh, i;

	if (pole_h > r.h - 6)
		pole_h = r.h - 6;
	if (pole_h < 6)
		pole_h = 6;
	fh = pole_h * 3 / 4;
	if (fh < 4)
		fh = 4;
	for (i = 0; i < fh; i++)
		ngui_fill_rect(ui, rect_make(cx + 1, top + i,
					     fh - i, 1), color);
	ngui_line(ui, cx, top, cx, top + pole_h, color);
	ngui_fill_rect(ui, rect_make(cx - 2, top + pole_h, 5, 2), color);
}

static void draw_cross(struct ngui *ui, struct ngui_rect r,
		       unsigned short color)
{
	int x0 = r.x + r.w / 2 - 5;
	int x1 = r.x + r.w / 2 + 5;
	int y0 = r.y + r.h / 2 - 5;
	int y1 = r.y + r.h / 2 + 5;

	ngui_line(ui, x0, y0, x1, y1, color);
	ngui_line(ui, x0, y1, x1, y0, color);
}

static unsigned short number_color(int n)
{
	switch (n) {
	case 1: return NGUI_RGB(120, 180, 255);
	case 2: return NGUI_RGB(120, 220, 140);
	case 3: return NGUI_RGB(255, 120, 120);
	case 4: return NGUI_RGB(180, 140, 255);
	case 5: return NGUI_RGB(255, 180, 100);
	case 6: return NGUI_RGB(100, 220, 220);
	case 7: return NGUI_RGB(230, 230, 230);
	default: return NGUI_RGB(160, 160, 170);
	}
}

static void draw_hidden(struct ngui *ui, struct ngui_rect r, int hot)
{
	int b = (r.w < 20) ? 1 : 2;

	ngui_fill_rect(ui, r, hot ? C_TILE_HOT : C_TILE);
	ngui_fill_rect(ui, rect_make(r.x, r.y, r.w, b), C_TILE_HI);
	ngui_fill_rect(ui, rect_make(r.x, r.y, b, r.h), C_TILE_HI);
	ngui_fill_rect(ui, rect_make(r.x, r.y + r.h - b, r.w, b), C_TILE_LO);
	ngui_fill_rect(ui, rect_make(r.x + r.w - b, r.y, b, r.h), C_TILE_LO);
}

static void draw_open(struct ngui *ui, struct ngui_rect r)
{
	ngui_fill_rect(ui, r, C_OPEN);
	ngui_fill_rect(ui, rect_make(r.x, r.y, r.w, 1), C_GRID);
	ngui_fill_rect(ui, rect_make(r.x, r.y, 1, r.h), C_GRID);
}

static void draw_face_icon(struct ngui *ui, struct ngui_rect r)
{
	int cx = r.x + r.w / 2;
	int cy = r.y + r.h / 2;
	int rad = (r.w < r.h ? r.w : r.h) / 2 - 3;

	if (rad < 6)
		rad = 6;
	fill_circle(ui, cx, cy, rad, C_FACE);

	if (won) {
		ngui_fill_rect(ui, rect_make(cx - 7, cy - 5, 6, 4), C_FACE_DK);
		ngui_fill_rect(ui, rect_make(cx + 1, cy - 5, 6, 4), C_FACE_DK);
		ngui_fill_rect(ui, rect_make(cx - 1, cy - 4, 2, 1), C_FACE_DK);
		ngui_line(ui, cx - 5, cy + 4, cx - 2, cy + 6, C_FACE_DK);
		ngui_line(ui, cx - 2, cy + 6, cx + 2, cy + 6, C_FACE_DK);
		ngui_line(ui, cx + 2, cy + 6, cx + 5, cy + 4, C_FACE_DK);
	} else if (over) {
		ngui_line(ui, cx - 6, cy - 5, cx - 2, cy - 1, C_FACE_DK);
		ngui_line(ui, cx - 2, cy - 5, cx - 6, cy - 1, C_FACE_DK);
		ngui_line(ui, cx + 2, cy - 5, cx + 6, cy - 1, C_FACE_DK);
		ngui_line(ui, cx + 6, cy - 5, cx + 2, cy - 1, C_FACE_DK);
		ngui_line(ui, cx - 5, cy + 6, cx - 2, cy + 4, C_FACE_DK);
		ngui_line(ui, cx - 2, cy + 4, cx + 2, cy + 4, C_FACE_DK);
		ngui_line(ui, cx + 2, cy + 4, cx + 5, cy + 6, C_FACE_DK);
	} else {
		ngui_fill_rect(ui, rect_make(cx - 5, cy - 4, 3, 4), C_FACE_DK);
		ngui_fill_rect(ui, rect_make(cx + 3, cy - 4, 3, 4), C_FACE_DK);
		ngui_line(ui, cx - 5, cy + 4, cx - 2, cy + 6, C_FACE_DK);
		ngui_line(ui, cx - 2, cy + 6, cx + 2, cy + 6, C_FACE_DK);
		ngui_line(ui, cx + 2, cy + 6, cx + 5, cy + 4, C_FACE_DK);
	}
}

static void draw_header(struct ngui *ui, const struct mines_layout *l)
{
	struct ngui_rect fr = face_rect(l);
	int hot = ui->inside && point_in(&fr, ui->mouse_x, ui->mouse_y);
	int down = hot && (ui->buttons & 1);
	unsigned short bg = down ? ui->theme.button_down :
			    hot ? ui->theme.button_hot : ui->theme.button;
	char buf[4];

	ngui_fill_rect(ui, rect_make(0, 0, ui->w, HEADER_H), C_PANEL);
	ngui_fill_rect(ui, rect_make(0, HEADER_H - 1, ui->w, 1), C_BORDER);

	format3(MINE_COUNT - flag_count, buf);
	ngui_text(ui, l->counter_x, l->counter_y, "M", C_DIM);
	ngui_text(ui, l->counter_x + 12, l->counter_y, buf, C_FLAG);

	format3(elapsed_seconds(), buf);
	ngui_text(ui, l->timer_x, l->timer_y, buf, C_WIN);
	ngui_text(ui, l->timer_x + 28, l->timer_y, "T", C_DIM);

	ngui_fill_rect(ui, fr, bg);
	ngui_frame(ui, fr, hot ? ui->theme.accent : C_BORDER);
	draw_face_icon(ui, rect_make(fr.x + 2, fr.y + 2,
				     fr.w - 4, fr.h - 4));
}

static void draw_board(struct ngui *ui, const struct mines_layout *l)
{
	int y, x;

	ngui_push_clip(ui, rect_make(l->bx, l->by, l->bw, l->bh));
	for (y = 0; y < MH; y++) {
		for (x = 0; x < MW; x++) {
			int idx = y * MW + x;
			struct ngui_rect r = rect_make(l->bx + x * l->cell,
						       l->by + y * l->cell,
						       l->cell, l->cell);
			int hot = ui->inside && point_in(&r, ui->mouse_x,
							 ui->mouse_y);

			if (opened[idx]) {
				draw_open(ui, r);
				if (mine[idx]) {
					draw_mine_icon(ui, r.x + r.w / 2,
						       r.y + r.h / 2,
						       r.w / 5, C_MINE);
				} else if (adj[idx]) {
					char nb[2];

					nb[0] = (char)('0' + adj[idx]);
					nb[1] = '\0';
					ngui_push_clip(ui, r);
					ngui_text(ui, r.x + (r.w - 8) / 2,
						  r.y + (r.h - 16) / 2, nb,
						  number_color(adj[idx]));
					ngui_pop_clip(ui);
				}
			} else {
				draw_hidden(ui, r, hot);
				if (flagged[idx]) {
					draw_flag_icon(ui, r, C_FLAG);
					if (over && !mine[idx])
						draw_cross(ui, r, C_LOSE);
				} else if (over && mine[idx]) {
					if (idx == exploded)
						ngui_fill_rect(ui, r, C_EXPLODED);
					draw_mine_icon(ui, r.x + r.w / 2,
						       r.y + r.h / 2,
						       r.w / 5, C_MINE);
				}
			}
		}
	}
	ngui_pop_clip(ui);
}

static void draw_footer(struct ngui *ui, const struct mines_layout *l)
{
	const char *msg;
	unsigned short color;
	int tw, x;

	if (won) {
		msg = "You win!  R: play again";
		color = C_WIN;
	} else if (over) {
		msg = "Boom!  R: play again";
		color = C_LOSE;
	} else if (started) {
		msg = "R: new   right-click: flag";
		color = C_DIM;
	} else {
		msg = "left: reveal  right: flag";
		color = C_DIM;
	}
	tw = (int)ngui_text_width_ui(ui, msg);
	x = (ui->w - tw) / 2;
	if (x < 4)
		x = 4;
	ngui_text(ui, x, l->footer_y, msg, color);
}

static void draw_app(struct ngui *ui, const struct mines_layout *l)
{
	ngui_fill_rect(ui, rect_make(0, 0, ui->w, ui->h), C_BG);
	ngui_fill_rect(ui, rect_make(0, ui->h - FOOTER_H, ui->w, 1), C_BORDER);
	draw_header(ui, l);
	draw_board(ui, l);
	draw_footer(ui, l);
}

static int handle_input(struct ngui *ui, const struct mines_layout *l)
{
	int changed = 0;
	int k;

	while ((k = ngui_key(ui)) != 0) {
		if (k == NGUI_KEY_ESC) {
			quit = 1;
		} else if (k == 'r' || k == 'R') {
			reset_game();
			changed = 1;
		}
	}

	if (ui->pressed) {
		struct ngui_rect fr = face_rect(l);

		if (point_in(&fr, ui->press_x, ui->press_y)) {
			reset_game();
			changed = 1;
		} else {
			int idx = cell_at(l, ui->press_x, ui->press_y);

			if (idx >= 0 && reveal_cell(idx))
				changed = 1;
		}
	}

	{
		unsigned int right = ui->buttons & 2u;

		if (right && !prev_right && ui->inside) {
			int idx = cell_at(l, ui->mouse_x, ui->mouse_y);

			if (idx >= 0 && toggle_flag(idx))
				changed = 1;
		}
		prev_right = right;
	}

	if (started && !over) {
		int sec = elapsed_seconds();

		if (sec != last_second) {
			last_second = sec;
			changed = 1;
		}
	}
	return changed;
}

void _start(const char *arg)
{
	struct win_info info;
	struct ngui ui;

	(void)arg;
	if (u_win_create(START_W, START_H, "mines", &info) != 0) {
		u_puts("mines: no graphics window\n");
		u_exit(1);
	}
	ngui_init(&ui, (unsigned short *)info.fb, (int)info.width,
		  (int)info.height);
	ngui_font_attach(&ui);
	reset_game();
	quit = 0;
	prev_right = 0;

	for (;;) {
		struct mines_layout l;
		int want, changed;

		want = ngui_begin(&ui);
		if (quit)
			break;
		compute_layout(&ui, &l);
		changed = handle_input(&ui, &l);
		if (want || changed) {
			draw_app(&ui, &l);
			ngui_end(&ui);
		} else {
			ngui_end_ex(&ui, 0);
		}
		if (quit)
			break;
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(0);
}
