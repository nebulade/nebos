/* solitaire: a Windows 95 style Klondike solitaire for a canvas window.
 *
 * Seven tableau piles, four foundations, a stock and a waste.  Draw one or
 * three, drag cards or whole runs, double-click to send a card to a
 * foundation, and click the stock to deal / recycle.  The look follows the
 * old Windows game: green felt, a grey Game/Help menu, white cards, a blue
 * patterned back and the bouncing cards after a win.
 *
 * Mouse only (the kernel keyboard path has no F2), so N starts a new game.
 * The window can be resized; the table is re-laid out.
 */

#include "ucmd_lib.h"
#include "ngui.h"

#define TABLEAU_PILES   7
#define FOUNDATION_PILES 4
#define RANK_COUNT      13
#define DECK_SIZE       52

#define MENU_H          22
#define STATUS_H        20

#define START_W         760
#define START_H         540

#define C_FELT          NGUI_RGB(0, 128, 0)
#define C_SLOT_FILL     NGUI_RGB(0, 112, 0)
#define C_SLOT_LINE     NGUI_RGB(0, 84, 0)
#define C_SLOT_ICON     NGUI_RGB(96, 190, 96)
#define C_MENU          NGUI_RGB(192, 192, 192)
#define C_MENU_TOP      NGUI_RGB(255, 255, 255)
#define C_MENU_DARK     NGUI_RGB(64, 64, 64)
#define C_MENU_HOT      NGUI_RGB(206, 206, 206)
#define C_TEXT          NGUI_RGB(0, 0, 0)
#define C_TEXT_LIGHT    NGUI_RGB(255, 255, 255)
#define C_NAVY          NGUI_RGB(0, 0, 128)
#define C_CARD          NGUI_RGB(255, 255, 255)
#define C_CARD_EDGE     NGUI_RGB(40, 40, 40)
#define C_SHADOW        NGUI_RGB(0, 82, 0)
#define C_RED           NGUI_RGB(200, 0, 0)
#define C_BLACK         NGUI_RGB(0, 0, 0)
#define C_GOLD          NGUI_RGB(184, 144, 64)
#define C_CREAM         NGUI_RGB(255, 248, 220)
#define C_SKIN          NGUI_RGB(255, 222, 189)
#define C_HAIR          NGUI_RGB(72, 48, 24)
#define C_HILITE        NGUI_RGB(255, 255, 0)

enum {
	SRC_NONE = 0,
	SRC_TABLEAU,
	SRC_WASTE,
	SRC_FOUNDATION
};

enum {
	TGT_NONE = 0,
	TGT_TABLEAU,
	TGT_FOUNDATION
};

enum {
	HIT_NONE = 0,
	HIT_STOCK,
	HIT_WASTE,
	HIT_TABLEAU,
	HIT_FOUNDATION
};

enum {
	MENU_NONE = 0,
	MENU_GAME,
	MENU_HELP
};

enum {
	HELP_NONE = 0,
	HELP_HOWTO,
	HELP_ABOUT
};

enum {
	ACT_NONE = 0,
	ACT_NEW,
	ACT_DRAW1,
	ACT_DRAW3,
	ACT_BLUE,
	ACT_RED,
	ACT_HOWTO,
	ACT_ABOUT
};

#define ID_MENU_GAME  0x101u
#define ID_MENU_HELP  0x102u
#define ID_MENU_ITEM  0x120u
#define ID_HELP_CLOSE 0x140u
#define ID_HELP_X     0x141u

#define BOUNCE_MAX 18

struct card_pile {
	unsigned char id[52];
	unsigned char up[52];
	int n;
};

struct sol_layout {
	int cw, ch;
	int gap;
	int col_x[7];
	int top_y;
	int tab_y;
	int status_y;
};

struct sel {
	int kind;
	int pile;
	int index;
};

struct target {
	int kind;
	int pile;
};

struct hit {
	int kind;
	int pile;
	int index;
};

struct menu_entry {
	int action;
	const char *label;
	const char *shortcut;
};

struct bounce_card {
	int active;
	int settled;
	int id;
	int x, y, vx, vy;
};

static struct card_pile stock;
static struct card_pile waste;
static struct card_pile foundation[FOUNDATION_PILES];
static struct card_pile tableau[TABLEAU_PILES];

static int draw_three;
static int deck_style;
static int score;
static int started;
static int won;
static int quit;

static unsigned int start_ms;
static unsigned int end_ms;
static unsigned int rng_state;
static unsigned int game_seq;

static struct sel select_sel;
static struct sel press_sel;
static struct sel drag_sel;
static struct target press_target;
static int press_active;
static int drag_active;
static int gesture_consumed;
static int press_x, press_y;
static int press_card_x, press_card_y;
static int drag_dx, drag_dy;

static unsigned int last_click_ms;
static struct sel last_click_sel;
static int last_click_valid;

static int menu_open;
static int help_open;
static int pending_action;
static int last_sec = -1;

static struct bounce_card bounce_cards[BOUNCE_MAX];
static int bounce_count;
static int bounce_active;
static int bounce_show;
static unsigned int bounce_start_ms;
static unsigned int bounce_spawn_ms;

static const struct menu_entry game_menu[] = {
	{ ACT_NEW,    "New game",   "N" },
	{ ACT_NONE,   0,            0   },
	{ ACT_DRAW1,  "Draw one",   0   },
	{ ACT_DRAW3,  "Draw three", 0   },
	{ ACT_NONE,   0,            0   },
	{ ACT_BLUE,   "Blue deck",  0   },
	{ ACT_RED,    "Red deck",   0   }
};

static const struct menu_entry help_menu[] = {
	{ ACT_HOWTO, "How to play",    0 },
	{ ACT_ABOUT, "About Solitaire", 0 }
};

static const char *const howto_lines[] = {
	"Goal: build each suit from Ace to King.",
	"Drag cards to move them.",
	"Drop a card on one rank higher and of",
	"the opposite colour.",
	"Empty tableau piles accept a King.",
	"Double-click a card to send it to a",
	"foundation.",
	"Click the deck to draw; click an empty",
	"deck to recycle the waste.",
	"N: new game      Esc: close"
};

static const char *const about_lines[] = {
	"Nebs OS edition",
	"",
	"A Windows 95 style Klondike.",
	"Drag, drop, and bounce."
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */

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

static void setup_theme(struct ngui *ui)
{
	ui->theme.panel = C_MENU;
	ui->theme.panel_alt = C_MENU_HOT;
	ui->theme.border = C_MENU_DARK;
	ui->theme.text = C_TEXT;
	ui->theme.text_dim = NGUI_RGB(88, 88, 88);
	ui->theme.accent = C_NAVY;
	ui->theme.selection = C_NAVY;
	ui->theme.button = C_MENU;
	ui->theme.button_hot = C_MENU_HOT;
	ui->theme.button_down = NGUI_RGB(160, 160, 160);
	ui->theme.error = C_RED;
}

static int clampi(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static int imin(int a, int b)
{
	return a < b ? a : b;
}

static int imax(int a, int b)
{
	return a > b ? a : b;
}

static int card_suit(int id)
{
	return id / RANK_COUNT;
}

static int card_rank(int id)
{
	return id % RANK_COUNT;
}

static int suit_is_red(int suit)
{
	return suit == 1 || suit == 2;
}

static const char *rank_text(int rank)
{
	static const char *const text[RANK_COUNT] = {
		"A", "2", "3", "4", "5", "6", "7",
		"8", "9", "10", "J", "Q", "K"
	};

	if (rank < 0 || rank >= RANK_COUNT)
		return "?";
	return text[rank];
}

static unsigned int text_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
}

static void fmt_int(char *out, int v)
{
	char tmp[12];
	int n = 0, o = 0;
	unsigned int uv;

	if (v < 0) {
		out[o++] = '-';
		uv = (unsigned int)(-v);
	} else {
		uv = (unsigned int)v;
	}
	do {
		tmp[n++] = (char)('0' + (uv % 10));
		uv /= 10;
	} while (uv);
	while (n > 0)
		out[o++] = tmp[--n];
	out[o] = '\0';
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

static void frame2(struct ngui *ui, struct ngui_rect r, unsigned short color)
{
	ngui_frame(ui, r, color);
	if (r.w > 4 && r.h > 4) {
		r.x++;
		r.y++;
		r.w -= 2;
		r.h -= 2;
		ngui_frame(ui, r, color);
	}
}

static void fill_circle(struct ngui *ui, int cx, int cy, int r,
			unsigned short color)
{
	int dy;

	if (r < 1)
		r = 1;
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

/* ------------------------------------------------------------------ */
/* layout                                                             */

static void compute_layout(struct ngui *ui, struct sol_layout *l)
{
	int margin, gap, cw, ch, max_ch, total, i;

	margin = clampi(ui->w / 64, 6, 16);
	gap = clampi(ui->w / 110, 4, 10);
	cw = (ui->w - 2 * margin - 6 * gap) / 7;
	cw = clampi(cw, 14, 92);
	ch = cw * 135 / 100;

	max_ch = (ui->h - MENU_H - STATUS_H - 30) / 2;
	if (max_ch < 18)
		max_ch = 18;
	if (ch > max_ch) {
		ch = max_ch;
		cw = ch * 100 / 135;
		if (cw < 14)
			cw = 14;
	}

	total = 7 * cw + 6 * gap;
	if (total > ui->w - 2) {
		if (gap > 2)
			gap = 2;
		total = 7 * cw + 6 * gap;
		if (total > ui->w - 2) {
			cw = (ui->w - 2 - 6 * gap) / 7;
			if (cw < 10)
				cw = 10;
			total = 7 * cw + 6 * gap;
		}
	}

	l->cw = cw;
	l->ch = ch;
	l->gap = gap;
	for (i = 0; i < 7; i++)
		l->col_x[i] = (ui->w - total) / 2 + i * (cw + gap);
	l->top_y = MENU_H + 8;
	l->tab_y = l->top_y + ch + 18;
	l->status_y = ui->h - STATUS_H;
	if (l->status_y < MENU_H + 20)
		l->status_y = MENU_H + 20;
	if (l->tab_y + ch > l->status_y - 2) {
		l->tab_y = l->status_y - ch - 2;
		if (l->tab_y < l->top_y + 4)
			l->tab_y = l->top_y + 4;
	}
}

static struct ngui_rect stock_rect(const struct sol_layout *l)
{
	return rect_make(l->col_x[0], l->top_y, l->cw, l->ch);
}

static struct ngui_rect waste_slot_rect(const struct sol_layout *l)
{
	return rect_make(l->col_x[1], l->top_y, l->cw, l->ch);
}

static struct ngui_rect foundation_rect(const struct sol_layout *l, int f)
{
	return rect_make(l->col_x[3 + f], l->top_y, l->cw, l->ch);
}

static struct ngui_rect tableau_slot_rect(const struct sol_layout *l, int p)
{
	return rect_make(l->col_x[p], l->tab_y, l->cw, l->ch);
}

static int waste_fan(const struct sol_layout *l)
{
	return imax(4, l->cw / 5);
}

static void pile_metrics(const struct card_pile *p,
			 const struct sol_layout *l, int *fd, int *fu)
{
	int down = 0, up, extra, base_extra;

	while (down < p->n && !p->up[down])
		down++;
	up = p->n - down;

	*fd = imax(4, l->ch / 10);
	*fu = imax(9, l->ch / 4);

	extra = l->status_y - l->tab_y - 2 - l->ch;
	if (extra < 4)
		extra = 4;
	base_extra = down * (*fd) + (up > 0 ? up - 1 : 0) * (*fu);
	if (base_extra > extra) {
		*fd = (*fd) * extra / base_extra;
		*fu = (*fu) * extra / base_extra;
		if (*fd < 2)
			*fd = 2;
		if (*fu < 4)
			*fu = 4;
	}
}

static int pile_y(const struct card_pile *p, const struct sol_layout *l,
		  int index)
{
	int fd, fu, y = l->tab_y, i;

	pile_metrics(p, l, &fd, &fu);
	for (i = 0; i < index && i < p->n; i++)
		y += p->up[i] ? fu : fd;
	return y;
}

/* ------------------------------------------------------------------ */
/* selection helpers                                                  */

static int sel_count(const struct sel *s)
{
	if (s->kind == SRC_TABLEAU)
		return tableau[s->pile].n - s->index;
	if (s->kind == SRC_WASTE || s->kind == SRC_FOUNDATION)
		return 1;
	return 0;
}

static int sel_first_id(const struct sel *s)
{
	if (s->kind == SRC_TABLEAU) {
		if (s->index < 0 || s->index >= tableau[s->pile].n)
			return -1;
		return tableau[s->pile].id[s->index];
	}
	if (s->kind == SRC_WASTE) {
		if (waste.n <= 0)
			return -1;
		return waste.id[waste.n - 1];
	}
	if (s->kind == SRC_FOUNDATION) {
		if (foundation[s->pile].n <= 0)
			return -1;
		return foundation[s->pile].id[foundation[s->pile].n - 1];
	}
	return -1;
}

static int sel_valid(const struct sel *s)
{
	int i, id, next;

	if (s->kind == SRC_NONE)
		return 0;
	if (s->kind == SRC_TABLEAU) {
		struct card_pile *p = &tableau[s->pile];

		if (s->pile < 0 || s->pile >= TABLEAU_PILES)
			return 0;
		if (s->index < 0 || s->index >= p->n)
			return 0;
		if (!p->up[s->index])
			return 0;
		for (i = s->index; i < p->n; i++) {
			if (!p->up[i])
				return 0;
			if (i + 1 < p->n) {
				id = p->id[i];
				next = p->id[i + 1];
				if (card_rank(next) != card_rank(id) - 1)
					return 0;
				if (suit_is_red(card_suit(next)) ==
				    suit_is_red(card_suit(id)))
					return 0;
			}
		}
		return 1;
	}
	if (s->kind == SRC_WASTE)
		return waste.n > 0;
	if (s->kind == SRC_FOUNDATION)
		return s->pile >= 0 && s->pile < FOUNDATION_PILES &&
		       foundation[s->pile].n > 0;
	return 0;
}

static int same_sel(const struct sel *a, const struct sel *b)
{
	return a->kind == b->kind && a->pile == b->pile &&
	       a->index == b->index;
}

static void clear_selection(void)
{
	select_sel.kind = SRC_NONE;
	select_sel.pile = 0;
	select_sel.index = 0;
}

static struct ngui_rect sel_rect(const struct sol_layout *l,
				 const struct sel *s)
{
	struct card_pile *p;

	if (s->kind == SRC_TABLEAU) {
		p = &tableau[s->pile];
		return rect_make(l->col_x[s->pile],
				 pile_y(p, l, s->index), l->cw, l->ch);
	}
	if (s->kind == SRC_WASTE) {
		int x = l->col_x[1];

		if (draw_three && waste.n > 1) {
			int start = waste.n > 3 ? waste.n - 3 : 0;
			int shown = waste.n - start;

			x += (shown - 1) * waste_fan(l);
		}
		return rect_make(x, l->top_y, l->cw, l->ch);
	}
	if (s->kind == SRC_FOUNDATION)
		return foundation_rect(l, s->pile);
	return rect_make(0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* hit testing and drop targets                                       */

static struct hit hit_test(const struct sol_layout *l, int x, int y)
{
	struct hit h;
	int i, f;

	h.kind = HIT_NONE;
	h.pile = 0;
	h.index = -1;

	{
		struct ngui_rect r = stock_rect(l);

		if (point_in(&r, x, y)) {
			h.kind = HIT_STOCK;
			return h;
		}
	}
	{
		struct ngui_rect r = waste_slot_rect(l);
		int fan = waste_fan(l);

		r.w += 2 * fan;
		if (point_in(&r, x, y)) {
			h.kind = HIT_WASTE;
			h.pile = 0;
			h.index = waste.n - 1;
			return h;
		}
	}
	for (f = 0; f < FOUNDATION_PILES; f++) {
		struct ngui_rect r = foundation_rect(l, f);

		if (point_in(&r, x, y)) {
			h.kind = HIT_FOUNDATION;
			h.pile = f;
			h.index = foundation[f].n - 1;
			return h;
		}
	}
	if (y >= l->tab_y && y < l->status_y) {
		for (i = 0; i < TABLEAU_PILES; i++) {
			struct card_pile *p = &tableau[i];

			if (x < l->col_x[i] || x >= l->col_x[i] + l->cw)
				continue;
			h.kind = HIT_TABLEAU;
			h.pile = i;
			h.index = -1;
			if (p->n == 0)
				return h;
			{
				int k;

				for (k = p->n - 1; k >= 0; k--) {
					int y0 = pile_y(p, l, k);
					int y1 = (k == p->n - 1) ? y0 + l->ch :
						 pile_y(p, l, k + 1);

					if (y >= y0 && y < y1) {
						h.index = k;
						return h;
					}
				}
			}
			return h;
		}
	}
	return h;
}

static struct sel selection_from_hit(const struct hit *h)
{
	struct sel s;

	s.kind = SRC_NONE;
	s.pile = 0;
	s.index = 0;
	if (h->kind == HIT_WASTE && waste.n > 0) {
		s.kind = SRC_WASTE;
		s.index = waste.n - 1;
	} else if (h->kind == HIT_FOUNDATION && h->index >= 0) {
		s.kind = SRC_FOUNDATION;
		s.pile = h->pile;
		s.index = foundation[h->pile].n - 1;
	} else if (h->kind == HIT_TABLEAU && h->index >= 0) {
		s.kind = SRC_TABLEAU;
		s.pile = h->pile;
		s.index = h->index;
	}
	if (!sel_valid(&s))
		s.kind = SRC_NONE;
	return s;
}

static struct target target_from_hit(const struct hit *h)
{
	struct target t;

	t.kind = TGT_NONE;
	t.pile = 0;
	if (h->kind == HIT_FOUNDATION) {
		t.kind = TGT_FOUNDATION;
		t.pile = h->pile;
	} else if (h->kind == HIT_TABLEAU) {
		t.kind = TGT_TABLEAU;
		t.pile = h->pile;
	}
	return t;
}

static struct target drop_target_at(const struct sol_layout *l, int x, int y)
{
	struct target t;
	int f, p;

	t.kind = TGT_NONE;
	t.pile = 0;

	if (y >= l->top_y - 10 && y <= l->top_y + l->ch + 10) {
		for (f = 0; f < FOUNDATION_PILES; f++) {
			int x0 = l->col_x[3 + f] - l->gap / 2;
			int x1 = l->col_x[3 + f] + l->cw + l->gap / 2;

			if (x >= x0 && x < x1) {
				t.kind = TGT_FOUNDATION;
				t.pile = f;
				return t;
			}
		}
	}
	if (y >= l->tab_y - 8 && y < l->status_y) {
		for (p = 0; p < TABLEAU_PILES; p++) {
			int x0 = l->col_x[p] - l->gap / 2;
			int x1 = l->col_x[p] + l->cw + l->gap / 2;

			if (x >= x0 && x < x1) {
				t.kind = TGT_TABLEAU;
				t.pile = p;
				return t;
			}
		}
	}
	return t;
}

static int can_drop(const struct sel *s, const struct target *t)
{
	int id, r, suit;

	if (s->kind == SRC_NONE || t->kind == TGT_NONE)
		return 0;
	if (!sel_valid(s))
		return 0;
	id = sel_first_id(s);
	if (id < 0)
		return 0;
	r = card_rank(id);
	suit = card_suit(id);

	if (t->kind == TGT_FOUNDATION) {
		struct card_pile *f;

		if (s->kind == SRC_FOUNDATION)
			return 0;
		if (sel_count(s) != 1)
			return 0;
		if (t->pile < 0 || t->pile >= FOUNDATION_PILES)
			return 0;
		f = &foundation[t->pile];
		if (f->n == 0)
			return r == 0;
		if (card_suit(f->id[f->n - 1]) != suit)
			return 0;
		return r == f->n;
	}

	if (t->kind == TGT_TABLEAU) {
		struct card_pile *p;
		int top;

		if (t->pile < 0 || t->pile >= TABLEAU_PILES)
			return 0;
		if (s->kind == SRC_TABLEAU && s->pile == t->pile)
			return 0;
		p = &tableau[t->pile];
		if (p->n == 0)
			return r == 12;
		if (!p->up[p->n - 1])
			return 0;
		top = p->id[p->n - 1];
		if (card_rank(top) != r + 1)
			return 0;
		return suit_is_red(card_suit(top)) != suit_is_red(suit);
	}
	return 0;
}

static struct ngui_rect target_rect(const struct sol_layout *l,
				    const struct target *t)
{
	if (t->kind == TGT_FOUNDATION)
		return foundation_rect(l, t->pile);
	if (t->kind == TGT_TABLEAU) {
		struct card_pile *p = &tableau[t->pile];

		if (p->n > 0)
			return rect_make(l->col_x[t->pile],
					 pile_y(p, l, p->n - 1), l->cw, l->ch);
		return tableau_slot_rect(l, t->pile);
	}
	return rect_make(0, 0, 0, 0);
}

static int hit_within_selection(const struct hit *h, const struct sel *s)
{
	if (s->kind == SRC_TABLEAU && h->kind == HIT_TABLEAU &&
	    h->pile == s->pile && h->index >= s->index)
		return 1;
	if (s->kind == SRC_WASTE && h->kind == HIT_WASTE)
		return 1;
	if (s->kind == SRC_FOUNDATION && h->kind == HIT_FOUNDATION &&
	    h->pile == s->pile)
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* game actions                                                       */

static void shuffle_deck(unsigned char *deck)
{
	int i;

	for (i = DECK_SIZE - 1; i > 0; i--) {
		int j = (int)(rng_next() % (unsigned int)(i + 1));
		unsigned char t = deck[i];

		deck[i] = deck[j];
		deck[j] = t;
	}
}

static void new_game(unsigned int now)
{
	unsigned char deck[DECK_SIZE];
	struct card_pile *p;
	int i, row, pos, f;

	game_seq++;
	rng_state = (u_ticks_ms() * 2654435761u) ^
		    (game_seq * 2246822519u) ^ 0x9E3779B9u;
	if (!rng_state)
		rng_state = 0x6D2B79F5u;

	for (i = 0; i < DECK_SIZE; i++)
		deck[i] = (unsigned char)i;
	shuffle_deck(deck);

	stock.n = 0;
	waste.n = 0;
	for (f = 0; f < FOUNDATION_PILES; f++) {
		foundation[f].n = 0;
		for (i = 0; i < 52; i++)
			foundation[f].up[i] = 1;
	}
	for (i = 0; i < TABLEAU_PILES; i++) {
		tableau[i].n = 0;
		for (f = 0; f < 52; f++)
			tableau[i].up[f] = 0;
	}

	pos = 0;
	for (row = 0; row < TABLEAU_PILES; row++) {
		for (i = row; i < TABLEAU_PILES; i++) {
			p = &tableau[i];
			p->id[p->n] = deck[pos++];
			p->up[p->n] = 0;
			p->n++;
		}
	}
	for (i = 0; i < TABLEAU_PILES; i++) {
		p = &tableau[i];
		if (p->n > 0)
			p->up[p->n - 1] = 1;
	}
	while (pos < DECK_SIZE) {
		stock.id[stock.n] = deck[pos++];
		stock.up[stock.n] = 0;
		stock.n++;
	}

	score = 0;
	started = 1;
	won = 0;
	start_ms = now;
	end_ms = 0;
	last_sec = -1;

	clear_selection();
	press_active = 0;
	drag_active = 0;
	gesture_consumed = 0;
	last_click_valid = 0;
	menu_open = MENU_NONE;
	help_open = HELP_NONE;
	pending_action = ACT_NONE;

	bounce_active = 0;
	bounce_show = 0;
	bounce_count = 0;
	for (i = 0; i < BOUNCE_MAX; i++) {
		bounce_cards[i].active = 0;
		bounce_cards[i].settled = 0;
	}
}

static int foundation_for_id(int id)
{
	int f, suit = card_suit(id), rank = card_rank(id);

	for (f = 0; f < FOUNDATION_PILES; f++) {
		if (foundation[f].n > 0 &&
		    card_suit(foundation[f].id[foundation[f].n - 1]) == suit)
			return f;
	}
	if (rank == 0) {
		for (f = 0; f < FOUNDATION_PILES; f++)
			if (foundation[f].n == 0)
				return f;
	}
	return -1;
}

static void start_bounce(unsigned int now)
{
	int i;

	for (i = 0; i < BOUNCE_MAX; i++) {
		bounce_cards[i].active = 0;
		bounce_cards[i].settled = 0;
	}
	bounce_count = 0;
	bounce_active = 1;
	bounce_show = 1;
	bounce_start_ms = now;
	bounce_spawn_ms = now;
}

static int check_win(unsigned int now)
{
	int f, total = 0;

	for (f = 0; f < FOUNDATION_PILES; f++)
		total += foundation[f].n;
	if (total == DECK_SIZE && !won) {
		won = 1;
		end_ms = now;
		clear_selection();
		start_bounce(now);
		return 1;
	}
	return 0;
}

static int perform_move(const struct sel *s, const struct target *t,
			unsigned int now)
{
	struct card_pile *src;
	unsigned char id;
	int i, count;

	if (!can_drop(s, t))
		return 0;

	count = sel_count(s);
	if (count <= 0)
		return 0;

	if (s->kind == SRC_TABLEAU) {
		src = &tableau[s->pile];
		id = src->id[s->index];
		if (t->kind == TGT_FOUNDATION) {
			struct card_pile *d = &foundation[t->pile];

			d->id[d->n] = id;
			d->up[d->n] = 1;
			d->n++;
			score += 10;
		} else {
			struct card_pile *d = &tableau[t->pile];

			for (i = 0; i < count; i++) {
				d->id[d->n] = src->id[s->index + i];
				d->up[d->n] = 1;
				d->n++;
			}
		}
		src->n = s->index;
		if (src->n > 0 && !src->up[src->n - 1]) {
			src->up[src->n - 1] = 1;
			score += 5;
		}
	} else if (s->kind == SRC_WASTE) {
		id = waste.id[waste.n - 1];
		waste.n--;
		if (t->kind == TGT_FOUNDATION) {
			struct card_pile *d = &foundation[t->pile];

			d->id[d->n] = id;
			d->up[d->n] = 1;
			d->n++;
			score += 10;
		} else {
			struct card_pile *d = &tableau[t->pile];

			d->id[d->n] = id;
			d->up[d->n] = 1;
			d->n++;
			score += 5;
		}
	} else if (s->kind == SRC_FOUNDATION) {
		src = &foundation[s->pile];
		id = src->id[src->n - 1];
		src->n--;
		if (t->kind == TGT_TABLEAU) {
			struct card_pile *d = &tableau[t->pile];

			d->id[d->n] = id;
			d->up[d->n] = 1;
			d->n++;
			score -= 15;
		}
	}

	clear_selection();
	drag_active = 0;
	last_click_valid = 0;
	check_win(now);
	return 1;
}

static int auto_to_foundation(const struct sel *s, unsigned int now)
{
	struct target t;
	int id, f;

	if (sel_count(s) != 1)
		return 0;
	id = sel_first_id(s);
	if (id < 0)
		return 0;
	f = foundation_for_id(id);
	if (f < 0)
		return 0;
	t.kind = TGT_FOUNDATION;
	t.pile = f;
	if (!can_drop(s, &t))
		return 0;
	return perform_move(s, &t, now);
}

static void stock_click(unsigned int now)
{
	int count;

	(void)now;
	if (won)
		return;
	if (stock.n > 0) {
		count = draw_three ? 3 : 1;
		while (count-- > 0 && stock.n > 0) {
			unsigned char id = stock.id[stock.n - 1];

			stock.n--;
			waste.id[waste.n] = id;
			waste.up[waste.n] = 1;
			waste.n++;
		}
	} else if (waste.n > 0) {
		while (waste.n > 0) {
			unsigned char id = waste.id[waste.n - 1];

			waste.n--;
			stock.id[stock.n] = id;
			stock.up[stock.n] = 0;
			stock.n++;
		}
		score += draw_three ? -20 : -100;
	}
	clear_selection();
	press_active = 0;
	last_click_valid = 0;
}

static int elapsed_seconds(unsigned int now)
{
	unsigned int end = won ? end_ms : now;

	if (!started)
		return 0;
	if ((int)(end - start_ms) < 0)
		return 0;
	return (int)((end - start_ms) / 1000u);
}

/* ------------------------------------------------------------------ */
/* card drawing                                                       */

static void fill_taper(struct ngui *ui, int cx, int y0, int y1,
		       int half0, int half1, unsigned short color)
{
	int t, y;

	if (y0 > y1) {
		t = y0;
		y0 = y1;
		y1 = t;
		t = half0;
		half0 = half1;
		half1 = t;
	}
	if (y1 <= y0)
		return;
	for (y = y0; y <= y1; y++) {
		int half = half0 + (half1 - half0) * (y - y0) / (y1 - y0);

		if (half > 0)
			ngui_fill_rect(ui,
				       rect_make(cx - half, y, half * 2 + 1, 1),
				       color);
	}
}

/* The familiar suit shapes.  rot=1 flips the glyph vertically for the
 * upside-down pip above a card's bottom-right index. */
static void draw_suit(struct ngui *ui, int cx, int cy, int suit,
		      unsigned short color, int size, int rot)
{
	int s = size;

	if (s < 7)
		s = 7;

	if (suit == 0) {
		/* spade: point up, rounded lobes below, small stem */
		int r = imax(2, s / 5);

		fill_circle(ui, cx - s / 5, cy + (rot ? -s / 8 : s / 8),
			    r, color);
		fill_circle(ui, cx + s / 5, cy + (rot ? -s / 8 : s / 8),
			    r, color);
		if (rot)
			fill_taper(ui, cx, cy + s / 2, cy - s / 6,
				   0, s / 2, color);
		else
			fill_taper(ui, cx, cy - s / 2, cy + s / 6,
				   0, s / 2, color);
		if (rot) {
			ngui_fill_rect(ui, rect_make(cx - s / 12,
						     cy - s / 5 - s / 5,
						     s / 6 + 1, s / 5),
				       color);
			ngui_fill_rect(ui, rect_make(cx - s / 5, cy - s / 2,
						     s * 2 / 5, 3), color);
		} else {
			ngui_fill_rect(ui, rect_make(cx - s / 12, cy + s / 5,
						     s / 6 + 1, s / 5),
				       color);
			ngui_fill_rect(ui, rect_make(cx - s / 5,
						     cy + s / 2 - 2,
						     s * 2 / 5, 3), color);
		}
	} else if (suit == 1) {
		/* heart */
		int r = imax(2, s / 4);
		int lobe_y = cy + (rot ? s / 6 : -s / 6);

		fill_circle(ui, cx - s / 5, lobe_y, r, color);
		fill_circle(ui, cx + s / 5, lobe_y, r, color);
		if (rot)
			fill_taper(ui, cx, cy - s / 2, lobe_y - r / 2,
				   0, s * 45 / 100, color);
		else
			fill_taper(ui, cx, lobe_y + r / 2, cy + s / 2,
				   s * 45 / 100, 0, color);
	} else if (suit == 2) {
		/* diamond */
		int hh = s / 2;
		int hw = s * 3 / 8;
		int y;

		for (y = -hh; y <= hh; y++) {
			int dy = y < 0 ? -y : y;
			int half = hw * (hh - dy) / hh;

			if (half > 0)
				ngui_fill_rect(ui,
					       rect_make(cx - half, cy + y,
							 half * 2 + 1, 1),
					       color);
		}
	} else {
		/* club */
		int r = imax(2, s / 5);

		fill_circle(ui, cx, cy + (rot ? s / 4 : -s / 4), r, color);
		fill_circle(ui, cx - s / 4, cy + (rot ? -s / 10 : s / 10),
			    r, color);
		fill_circle(ui, cx + s / 4, cy + (rot ? -s / 10 : s / 10),
			    r, color);
		if (rot) {
			ngui_fill_rect(ui, rect_make(cx - s / 12,
						     cy - s / 5 - s / 5,
						     s / 6 + 1, s / 5),
				       color);
			ngui_fill_rect(ui, rect_make(cx - s / 5, cy - s / 2,
						     s * 2 / 5, 3), color);
		} else {
			ngui_fill_rect(ui, rect_make(cx - s / 12, cy + s / 5,
						     s / 6 + 1, s / 5),
				       color);
			ngui_fill_rect(ui, rect_make(cx - s / 5,
						     cy + s / 2 - 2,
						     s * 2 / 5, 3), color);
		}
	}
}

static int card_notch(const struct ngui_rect *r)
{
	int n = r->w / 30;

	if (n < 2)
		n = 2;
	if (n > 4)
		n = 4;
	return n;
}

static struct ngui_rect inset_rect(struct ngui_rect r, int n)
{
	if (r.w > 2 * n && r.h > 2 * n) {
		r.x += n;
		r.y += n;
		r.w -= 2 * n;
		r.h -= 2 * n;
	}
	return r;
}

/* White playing cards have a small square cut out of each corner.  Filling a
 * plain rectangle would paint over the card underneath, so build the shape
 * from three bands. */
static void fill_card_shape(struct ngui *ui, struct ngui_rect r,
			    unsigned short color, int notch)
{
	if (notch <= 0 || r.w <= 2 * notch || r.h <= 2 * notch) {
		ngui_fill_rect(ui, r, color);
		return;
	}
	ngui_fill_rect(ui, rect_make(r.x + notch, r.y,
				     r.w - 2 * notch, notch), color);
	ngui_fill_rect(ui, rect_make(r.x, r.y + notch,
				     r.w, r.h - 2 * notch), color);
	ngui_fill_rect(ui, rect_make(r.x + notch, r.y + r.h - notch,
				     r.w - 2 * notch, notch), color);
}

struct pip_pos {
	unsigned short x, y;
};

static const struct pip_pos pips2[] = {
	{ 500, 260 }, { 500, 740 }
};
static const struct pip_pos pips3[] = {
	{ 500, 260 }, { 500, 500 }, { 500, 740 }
};
static const struct pip_pos pips4[] = {
	{ 320, 260 }, { 680, 260 }, { 320, 740 }, { 680, 740 }
};
static const struct pip_pos pips5[] = {
	{ 320, 260 }, { 680, 260 }, { 500, 500 },
	{ 320, 740 }, { 680, 740 }
};
static const struct pip_pos pips6[] = {
	{ 320, 260 }, { 680, 260 }, { 320, 500 }, { 680, 500 },
	{ 320, 740 }, { 680, 740 }
};
static const struct pip_pos pips7[] = {
	{ 320, 260 }, { 680, 260 }, { 500, 380 },
	{ 320, 500 }, { 680, 500 }, { 320, 740 }, { 680, 740 }
};
static const struct pip_pos pips8[] = {
	{ 320, 260 }, { 680, 260 }, { 500, 380 }, { 320, 500 },
	{ 680, 500 }, { 500, 620 }, { 320, 740 }, { 680, 740 }
};
static const struct pip_pos pips9[] = {
	{ 320, 220 }, { 680, 220 }, { 320, 390 }, { 680, 390 },
	{ 500, 500 }, { 320, 610 }, { 680, 610 },
	{ 320, 780 }, { 680, 780 }
};
static const struct pip_pos pips10[] = {
	{ 320, 220 }, { 680, 220 }, { 320, 390 }, { 680, 390 },
	{ 500, 305 }, { 500, 695 }, { 320, 610 }, { 680, 610 },
	{ 320, 780 }, { 680, 780 }
};

static void draw_pips(struct ngui *ui, const struct ngui_rect *r, int rank,
		      int suit, unsigned short ink)
{
	const struct pip_pos *p = 0;
	int n = 0, i, ps;

	switch (rank) {
	case 1: p = pips2; n = 2; break;
	case 2: p = pips3; n = 3; break;
	case 3: p = pips4; n = 4; break;
	case 4: p = pips5; n = 5; break;
	case 5: p = pips6; n = 6; break;
	case 6: p = pips7; n = 7; break;
	case 7: p = pips8; n = 8; break;
	case 8: p = pips9; n = 9; break;
	case 9: p = pips10; n = 10; break;
	default: return;
	}
	ps = clampi(r->w * 20 / 100, 7, 20);
	for (i = 0; i < n; i++) {
		int cx = r->x + (int)r->w * p[i].x / 1000;
		int cy = r->y + (int)r->h * p[i].y / 1000;

		draw_suit(ui, cx, cy, suit, ink, ps, 0);
	}
}

static void draw_card_corner(struct ngui *ui, const struct ngui_rect *r,
			     int suit, unsigned short ink,
			     const char *rt, int rl, int top)
{
	int cs = clampi(r->w / 8, 7, 12);
	int x, y, cx, cy;

	if (top) {
		x = r->x + 3;
		y = r->y + 2;
		ngui_text_styled(ui, x, y, rt, (unsigned int)rl, ink, 1, 1);
		cx = x + (rl * 8) / 2;
		cy = y + 17 + cs / 2;
		if (cy + cs / 2 > r->y + r->h - 2)
			cy = r->y + r->h - 2 - cs / 2;
		draw_suit(ui, cx, cy, suit, ink, cs, 0);
	} else {
		x = r->x + r->w - 3 - rl * 8;
		y = r->y + r->h - 18;
		if (y < r->y + 20)
			y = r->y + 20;
		ngui_text_rot180(ui, x, y, rt, (unsigned int)rl, ink);
		cx = r->x + r->w - 4 - cs / 2;
		cy = y - 2 - cs / 2;
		if (cy - cs / 2 < r->y + 2)
			cy = r->y + 2 + cs / 2;
		draw_suit(ui, cx, cy, suit, ink, cs, 1);
	}
}

static const char *const court_king[48] = {
	"........YYY....YYY....YYY.......",
	"........YYY....YYY....YYY.......",
	"........YYY....YYY....YYY.......",
	"........#################.......",
	"........#YYRRYYYRRYYYRRY#.......",
	"........#################.......",
	".........HHHHHHHHHHHHHH.........",
	".........HHHHHHHHHHHHHH.........",
	"........HHHHHHHHHHHHHHHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSS##SSSS##SSHH........",
	"........HHSS##SSSS##SSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSggggSggggSHH........",
	"........HHSSggggRggggSHH........",
	"........Hggggggggggggggg........",
	"........Hggggggggggggggg........",
	"........Hggggggggggggggg........",
	".........ggggggggggggggg........",
	".........ggggggggggggggg........",
	"..........############..........",
	"..........#WWWWWWWWWW#..........",
	"..........#WWWWWWWWWW#..........",
	"..........############..........",
	"........#RRRRRRRRRRRRRRR#.......",
	"........#RYRRRRRRRRRRRYR#.......",
	"....#####YRRRRRRRRRRRRRY#####...",
	"....#RRR#YRRRRRRRRRRRRRY#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#YYYYYRRRRRRRRRRRRRYYYYY#...",
	"...##YYYYYRRRRRRRRRRRRRYYYYY##..",
	"...#SS#RRRRRRRRRRRRRRRRRRR#SS#..",
	"...#SS#R#YYYYYRRRRYYYYY#RR#SS#..",
	"..##SS#R#YYYYYRRRRYYYYY#RR#SS##.",
	".#R####R################RR####R#",
	".#RYRRRRRRRRRRRRRRRRRRRRRRRRRYR#",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"RRRRRR####################RRRRRR",
	"......#RRRRRRRRRRRRRRRRRR#......",
	"......####################......",
	"................................",
};

static const char *const court_queen[48] = {
	"..........YWY..YWY..YWY.........",
	"..........YYY..YYY..YYY.........",
	"..........YYY..YYY..YYY.........",
	".........#YYY#RRRR##YYY#........",
	".........#YYYYRRRRYYYYY#........",
	".........###############........",
	".........HHHHHHHHHHHHHH.........",
	".........HHHHHHHHHHHHHH.........",
	".......HHHHHHHHHHHHHHHHHH.......",
	".......HHHSSSSSSSSSSSSHHH.......",
	".......HHHSSSSSSSSSSSSHHH.......",
	".......HHHSSSSSSSSSSSSHHH.......",
	".......HHHSS##SSSS##SSHHH.......",
	".......HHHSS##SSSS##SSHHH.......",
	".......HHHSSSSS##SSSSSHHH.......",
	".......HHHSSSSS##SSSSSHHH.......",
	".......HHHSSSSS##SSSSSHHH.......",
	".......HHHSSSSSSSSSSSSHHH.......",
	".......HHHSSSRRRRRRSSSHHH.......",
	".......HHHSSSRRRRRRSSSHHH.......",
	".......HHHSSSSSSSSSSSSHHH.......",
	".......HHH############HHH.......",
	".......HHH..SSSSSSSS..HHH.......",
	".......HHH..SSSSSSSS..HHH.......",
	".......HHH############HHH.......",
	".......HHH#WWWWWWWWWW#HHH.......",
	".......HHH#WWWWWWWWWW#HHH.......",
	".......HHH############HHH.......",
	"........#RRRRRRRRRRRRRRR#.......",
	"........#RYRRRRRRRRRRRYR#.......",
	"....#####YRRRRRRRRRRRRRY#####...",
	"....#RRR#YRRRRRRRRRRRRRY#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#YYYYYRRRRRRRRRRRRRYYYYY#...",
	"...##YYYYYRRRRRRRRRRRRRYYYYY##..",
	"...#SS#RRRRRRRRRRRRRRRRRRR#SS#..",
	"...#SS#R#YYYYYRRRRYYYYY#RR#SS#..",
	"..##SS#R#YYYYYRRRRYYYYY#RR#SS##.",
	".#R####R################RR####R#",
	".#RYRRRRRRRRRRRRRRRRRRRRRRRRRYR#",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"RRRRRR####################RRRRRR",
	"......#RRRRRRRRRRRRRRRRRR#......",
	"......####################......",
	"................................",
};

static const char *const court_jack[48] = {
	".......................RRY......",
	".......................RRRY.....",
	"........###############RRR......",
	"........#RRRRRRRRRRRRRRRRR......",
	"........#RRRRRRRRRRRRRRRRR......",
	"........#RRRRRRRRRRRRRRR#.......",
	"........#RRRRRRRRRRRRRRR#.......",
	"........YYYYYYYYYYYYYYYYY.......",
	"........HHHHHHHHHHHHHHHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSS##SSSS##SSHH........",
	"........HHSS##SSSS##SSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSSSS##SSSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HHSSSRRRRRRSSSHH........",
	"........HHSSSRRRRRRSSSHH........",
	"........HHSSSSSSSSSSSSHH........",
	"........HH############HH........",
	"............SSSSSSSS............",
	"............SSSSSSSS............",
	"..........############..........",
	"..........#WWWWWWWWWW#..........",
	"..........#WWWWYYWWWW#..........",
	"..........#####YY#####..........",
	"........#RRRRRRRRRRRRRRR#.......",
	"........#RYRRRRRRRRRRRYR#.......",
	"....#####YRRRRRRRRRRRRRY#####...",
	"....#RRR#YRRRRRRRRRRRRRY#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#RRR#RRRRRRRRRRRRRRR#RRR#...",
	"....#YYYYYRRRRRRRRRRRRRYYYYY#...",
	"...##YYYYYRRRRRRRRRRRRRYYYYY##..",
	"...#SS#RRRRRRRRRRRRRRRRRRR#SS#..",
	"...#SS#R#YYYYYRRRRYYYYY#RR#SS#..",
	"..##SS#R#YYYYYRRRRYYYYY#RR#SS##.",
	".#R####R################RR####R#",
	".#RYRRRRRRRRRRRRRRRRRRRRRRRRRYR#",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"#RYRRRRRRRRRRRRRRRRRRRRRRRRRRRYR",
	"RRRRRR####################RRRRRR",
	"......#RRRRRRRRRRRRRRRRRR#......",
	"......####################......",
	"................................",
};

static const char *const *court_rows(int rank)
{
	if (rank == 12)
		return court_king;
	if (rank == 11)
		return court_queen;
	if (rank == 10)
		return court_jack;
	return 0;
}

static unsigned short court_color(char c)
{
	switch (c) {
	case '#': return C_BLACK;
	case 'W': return C_CARD;
	case 'S': return C_SKIN;
	case 'H': return C_HAIR;
	case 'Y': return C_GOLD;
	case 'R': return C_RED;
	case 'g': return NGUI_RGB(184, 184, 184);
	default: return 0;
	}
}

/* Blit the 32x48 court portrait with one fill per same-colour run. */
static void draw_court_bitmap(struct ngui *ui, int px, int py, int pw, int ph,
			      int rank)
{
	const char *const *rows = court_rows(rank);
	int x, y;

	if (!rows)
		return;
	for (y = 0; y < 48; y++) {
		int y0 = py + y * ph / 48;
		int y1 = py + (y + 1) * ph / 48;

		for (x = 0; x < 32; ) {
			char ch = rows[y][x];
			int x2, x0, x1;
			unsigned short color;

			if (ch == '.') {
				x++;
				continue;
			}
			x2 = x;
			while (x2 + 1 < 32 && rows[y][x2 + 1] == ch)
				x2++;
			color = court_color(ch);
			if (color) {
				x0 = px + x * pw / 32;
				x1 = px + (x2 + 1) * pw / 32;
				ngui_fill_rect(ui, rect_make(x0, y0,
							     x1 - x0, y1 - y0),
					       color);
			}
			x = x2 + 1;
		}
	}
}

static void draw_court_card(struct ngui *ui, const struct ngui_rect *r,
			    int rank, int suit, unsigned short ink)
{
	int px = r->x + r->w * 19 / 100;
	int py = r->y + r->h * 11 / 100;
	int pw = r->w * 62 / 100;
	int ph = r->h * 78 / 100;

	if (pw < 24 || ph < 40) {
		int ss = imin(r->w * 45 / 100, r->h * 30 / 100);

		ss = clampi(ss, 9, 40);
		draw_suit(ui, r->x + r->w / 2, r->y + r->h / 2 + 2,
			  suit, ink, ss, 0);
		return;
	}
	ngui_fill_rect(ui, rect_make(px, py, pw, ph), C_CREAM);
	draw_court_bitmap(ui, px, py, pw, ph, rank);
	ngui_frame(ui, rect_make(px, py, pw, ph), C_BLACK);
}

static void draw_card_face_content(struct ngui *ui, struct ngui_rect r, int id)
{
	int rank = card_rank(id);
	int suit = card_suit(id);
	unsigned short ink = suit_is_red(suit) ? C_RED : C_BLACK;
	const char *rt = rank_text(rank);
	int rl = (int)text_len(rt);
	int notch = card_notch(&r);

	fill_card_shape(ui, r, C_CARD_EDGE, notch);
	fill_card_shape(ui, inset_rect(r, 1), C_CARD, imax(0, notch - 1));
	ngui_push_clip(ui, r);

	if (rank >= 10) {
		draw_court_card(ui, &r, rank, suit, ink);
	} else if (rank == 0) {
		int ss = imin(r.w * 45 / 100, r.h * 30 / 100);

		ss = clampi(ss, 9, 40);
		draw_suit(ui, r.x + r.w / 2, r.y + r.h / 2 + 2,
			  suit, ink, ss, 0);
	} else if (r.w < 30) {
		int ss = imin(r.w * 42 / 100, r.h * 28 / 100);

		ss = clampi(ss, 8, 30);
		draw_suit(ui, r.x + r.w / 2, r.y + r.h / 2 + 2,
			  suit, ink, ss, 0);
	} else {
		draw_pips(ui, &r, rank, suit, ink);
	}

	draw_card_corner(ui, &r, suit, ink, rt, rl, 1);
	draw_card_corner(ui, &r, suit, ink, rt, rl, 0);
	ngui_pop_clip(ui);
}

/* The Windows Solitaire back is a little castle on a night hill. */
static void draw_card_back_content(struct ngui *ui, struct ngui_rect r)
{
	struct ngui_rect inner;
	unsigned short sky, sky2, moon, hill, tower, tower_dark, lit;
	int notch = card_notch(&r);
	int mx, my, mr, base, keep_w, keep_h, kx, ky, i, tw, th;

	if (deck_style == 1) {
		sky = NGUI_RGB(96, 0, 0);
		sky2 = NGUI_RGB(128, 24, 24);
		moon = NGUI_RGB(255, 208, 128);
		hill = NGUI_RGB(48, 0, 0);
		tower = NGUI_RGB(224, 150, 128);
		tower_dark = NGUI_RGB(104, 40, 40);
		lit = NGUI_RGB(255, 232, 144);
	} else {
		sky = NGUI_RGB(0, 0, 96);
		sky2 = NGUI_RGB(0, 28, 128);
		moon = NGUI_RGB(236, 236, 168);
		hill = NGUI_RGB(0, 0, 48);
		tower = NGUI_RGB(184, 196, 220);
		tower_dark = NGUI_RGB(56, 68, 112);
		lit = NGUI_RGB(255, 232, 144);
	}

	fill_card_shape(ui, r, C_CARD_EDGE, notch);
	fill_card_shape(ui, inset_rect(r, 1), C_CARD, imax(0, notch - 1));
	inner = inset_rect(r, 3);
	if (inner.w < 6 || inner.h < 6)
		return;

	ngui_fill_rect(ui, inner, sky);
	ngui_fill_rect(ui, rect_make(inner.x, inner.y + inner.h * 58 / 100,
				     inner.w, inner.h * 42 / 100), sky2);

	mx = inner.x + inner.w * 74 / 100;
	my = inner.y + inner.h * 22 / 100;
	mr = imax(2, inner.w / 10);
	fill_circle(ui, mx, my, mr, moon);
	fill_circle(ui, mx + mr / 2, my - mr / 3, mr * 4 / 5, sky);

	{
		static const unsigned char star_x[] = { 18, 34, 48, 62, 24, 84 };
		static const unsigned char star_y[] = { 12, 30, 16, 34, 48, 40 };

		for (i = 0; i < 6; i++) {
			int sx = inner.x + inner.w * star_x[i] / 100;
			int sy = inner.y + inner.h * star_y[i] / 100;

			ngui_fill_rect(ui, rect_make(sx, sy, 1, 1), moon);
		}
	}

	base = inner.y + inner.h * 76 / 100;
	ngui_fill_rect(ui, rect_make(inner.x, base, inner.w,
				     inner.y + inner.h - base), hill);

	if (inner.w < 26 || inner.h < 34) {
		ngui_fill_rect(ui, rect_make(inner.x + inner.w / 2 - 2,
					     base - inner.h / 4, 5,
					     inner.h / 4), tower);
		ngui_frame(ui, inner, tower_dark);
		return;
	}

	/* central keep */
	keep_w = inner.w * 34 / 100;
	keep_h = inner.h * 34 / 100;
	kx = inner.x + (inner.w - keep_w) / 2;
	ky = base - keep_h;
	ngui_fill_rect(ui, rect_make(kx, ky, keep_w, keep_h), tower);
	ngui_frame(ui, rect_make(kx, ky, keep_w, keep_h), tower_dark);
	for (i = 0; i < 3; i++)
		ngui_fill_rect(ui, rect_make(kx + i * keep_w / 3 + 1,
					     ky - 3, keep_w / 5, 3), tower);
	ngui_fill_rect(ui, rect_make(kx + keep_w / 2 - 4,
				     ky + keep_h * 35 / 100, 3, 5), lit);
	ngui_fill_rect(ui, rect_make(kx + keep_w / 2 + 2,
				     ky + keep_h * 35 / 100, 3, 5), lit);
	ngui_fill_rect(ui, rect_make(kx + keep_w / 2 - 4, base - 12,
				     9, 12), tower_dark);

	/* side towers */
	tw = inner.w * 18 / 100;
	th = inner.h * 26 / 100;
	for (i = 0; i < 2; i++) {
		int tx = i ? inner.x + inner.w - tw - inner.w * 12 / 100 :
			     inner.x + inner.w * 12 / 100;

		ngui_fill_rect(ui, rect_make(tx, base - th, tw, th), tower);
		ngui_frame(ui, rect_make(tx, base - th, tw, th), tower_dark);
		ngui_fill_rect(ui, rect_make(tx + tw / 2 - 1,
					     base - th + 4, 3, 4), lit);
		ngui_fill_rect(ui, rect_make(tx + tw / 4, base - th - 3,
					     tw / 2, 3), tower);
	}

	/* flag */
	ngui_line(ui, kx + keep_w / 2, ky - 10, kx + keep_w / 2, ky - 3,
		  tower_dark);
	ngui_fill_rect(ui, rect_make(kx + keep_w / 2 + 1, ky - 10,
				     5, 3), C_RED);

	ngui_frame(ui, inner, tower_dark);
}

static void draw_card(struct ngui *ui, struct ngui_rect r, int id, int up)
{
	int notch = card_notch(&r);

	fill_card_shape(ui, rect_make(r.x + 2, r.y + 2, r.w, r.h),
			C_SHADOW, notch);
	if (up)
		draw_card_face_content(ui, r, id);
	else
		draw_card_back_content(ui, r);
}

static void draw_empty_slot(struct ngui *ui, struct ngui_rect r)
{
	ngui_fill_rect(ui, r, C_SLOT_FILL);
	ngui_frame(ui, r, C_SLOT_LINE);
}

static void draw_recycle_icon(struct ngui *ui, struct ngui_rect r)
{
	int cx = r.x + r.w / 2;
	int cy = r.y + r.h / 2;
	int rad = imin(r.w, r.h) / 5;
	int q = rad * 7 / 10;

	if (rad < 5)
		return;
	ngui_line(ui, cx, cy - rad, cx + q, cy - q, C_SLOT_ICON);
	ngui_line(ui, cx + q, cy - q, cx + rad, cy, C_SLOT_ICON);
	ngui_line(ui, cx + rad, cy, cx + q, cy + q, C_SLOT_ICON);
	ngui_line(ui, cx + q, cy + q, cx, cy + rad, C_SLOT_ICON);
	ngui_line(ui, cx, cy + rad, cx - q, cy + q, C_SLOT_ICON);
	ngui_line(ui, cx - q, cy + q, cx - rad, cy, C_SLOT_ICON);
	ngui_line(ui, cx - rad, cy, cx - q, cy - q, C_SLOT_ICON);
	ngui_line(ui, cx - q, cy - q, cx - rad / 2, cy - rad, C_SLOT_ICON);
	ngui_line(ui, cx, cy - rad, cx + rad / 2, cy - rad, C_SLOT_ICON);
	ngui_line(ui, cx, cy - rad, cx, cy - rad / 2, C_SLOT_ICON);
}

/* ------------------------------------------------------------------ */
/* table drawing                                                      */

static void draw_stock(struct ngui *ui, const struct sol_layout *l)
{
	struct ngui_rect r = stock_rect(l);

	if (stock.n > 0) {
		draw_card(ui, r, stock.id[stock.n - 1], 0);
	} else {
		draw_empty_slot(ui, r);
		if (waste.n > 0)
			draw_recycle_icon(ui, r);
	}
}

static void draw_waste(struct ngui *ui, const struct sol_layout *l)
{
	struct ngui_rect r = waste_slot_rect(l);
	int fan = waste_fan(l);
	int top = waste.n;
	int start, i;

	if (drag_active && drag_sel.kind == SRC_WASTE)
		top--;

	if (top <= 0) {
		draw_empty_slot(ui, r);
		return;
	}
	if (draw_three)
		start = top > 3 ? top - 3 : 0;
	else
		start = top - 1;

	for (i = start; i < top; i++) {
		struct ngui_rect cr = r;

		cr.x += (i - start) * fan;
		draw_card(ui, cr, waste.id[i], 1);
	}
}

static void draw_foundations(struct ngui *ui, const struct sol_layout *l)
{
	int f;

	for (f = 0; f < FOUNDATION_PILES; f++) {
		struct ngui_rect r = foundation_rect(l, f);
		int top = foundation[f].n;

		if (drag_active && drag_sel.kind == SRC_FOUNDATION &&
		    drag_sel.pile == f)
			top--;
		if (top > 0)
			draw_card(ui, r, foundation[f].id[top - 1], 1);
		else
			draw_empty_slot(ui, r);
	}
}

static void draw_tableau(struct ngui *ui, const struct sol_layout *l)
{
	int p, i;

	for (p = 0; p < TABLEAU_PILES; p++) {
		struct card_pile *pile = &tableau[p];
		int n = pile->n;

		if (drag_active && drag_sel.kind == SRC_TABLEAU &&
		    drag_sel.pile == p)
			n = drag_sel.index;
		if (n <= 0) {
			draw_empty_slot(ui, tableau_slot_rect(l, p));
			continue;
		}
		for (i = 0; i < n; i++) {
			struct ngui_rect r;

			r = rect_make(l->col_x[p], pile_y(pile, l, i),
				      l->cw, l->ch);
			draw_card(ui, r, pile->id[i], pile->up[i]);
		}
	}
}

static void draw_drop_highlight(struct ngui *ui, const struct sol_layout *l)
{
	const struct sel *s = 0;
	struct target t;
	struct ngui_rect r;

	if (drag_active)
		s = &drag_sel;
	else if (select_sel.kind != SRC_NONE)
		s = &select_sel;
	if (!s)
		return;
	t = drop_target_at(l, ui->mouse_x, ui->mouse_y);
	if (!can_drop(s, &t))
		return;
	r = target_rect(l, &t);
	if (r.w > 0 && r.h > 0)
		frame2(ui, r, C_HILITE);
}

static void draw_selection_outline(struct ngui *ui, const struct sol_layout *l)
{
	struct ngui_rect r;

	if (drag_active || select_sel.kind == SRC_NONE)
		return;
	if (select_sel.kind == SRC_TABLEAU) {
		struct card_pile *p = &tableau[select_sel.pile];
		int y0 = pile_y(p, l, select_sel.index);
		int y1 = pile_y(p, l, p->n - 1) + l->ch;

		r = rect_make(l->col_x[select_sel.pile], y0, l->cw, y1 - y0);
	} else {
		r = sel_rect(l, &select_sel);
	}
	frame2(ui, r, C_HILITE);
}

static void draw_drag_cards(struct ngui *ui, const struct sol_layout *l)
{
	struct card_pile *p;
	int count, i, step, x, y;

	if (!drag_active || drag_sel.kind == SRC_NONE)
		return;
	count = sel_count(&drag_sel);
	if (count <= 0)
		return;
	x = ui->mouse_x - drag_dx;
	y = ui->mouse_y - drag_dy;
	step = imax(8, l->ch / 5);
	for (i = 0; i < count; i++) {
		struct ngui_rect r = rect_make(x, y + i * step, l->cw, l->ch);

		if (drag_sel.kind == SRC_TABLEAU) {
			p = &tableau[drag_sel.pile];
			draw_card(ui, r, p->id[drag_sel.index + i], 1);
		} else if (drag_sel.kind == SRC_WASTE) {
			draw_card(ui, r, waste.id[waste.n - 1], 1);
		} else {
			p = &foundation[drag_sel.pile];
			draw_card(ui, r, p->id[p->n - 1], 1);
		}
	}
}

static void draw_bounce_cards(struct ngui *ui, const struct sol_layout *l)
{
	int i;

	if (!bounce_show)
		return;
	for (i = 0; i < BOUNCE_MAX; i++) {
		struct bounce_card *b = &bounce_cards[i];

		if (!b->active && !b->settled)
			continue;
		draw_card(ui, rect_make(b->x, b->y, l->cw, l->ch), b->id, 1);
	}
}

/* ------------------------------------------------------------------ */
/* menu, status and help                                              */

static int menu_entries(int which, const struct menu_entry **out)
{
	if (which == MENU_GAME) {
		*out = game_menu;
		return (int)(sizeof(game_menu) / sizeof(game_menu[0]));
	}
	*out = help_menu;
	return (int)(sizeof(help_menu) / sizeof(help_menu[0]));
}

static int menu_entry_h(const struct menu_entry *e)
{
	return e->label ? 21 : 7;
}

static int menu_dropdown_h(int which)
{
	const struct menu_entry *e;
	int n, i, h = 6;

	n = menu_entries(which, &e);
	for (i = 0; i < n; i++)
		h += menu_entry_h(&e[i]);
	return h;
}

static int menu_x(int which)
{
	return which == MENU_GAME ? 4 : 52;
}

static int menu_w(int which)
{
	return which == MENU_GAME ? 172 : 158;
}

static int action_checked(int action)
{
	switch (action) {
	case ACT_DRAW1:
		return !draw_three;
	case ACT_DRAW3:
		return draw_three;
	case ACT_BLUE:
		return deck_style == 0;
	case ACT_RED:
		return deck_style == 1;
	default:
		return 0;
	}
}

static int menu_bar_button(struct ngui *ui, unsigned int id,
			   struct ngui_rect r, const char *label, int active)
{
	unsigned short ob = ui->theme.button;
	unsigned short oh = ui->theme.button_hot;
	unsigned short od = ui->theme.button_down;
	unsigned short obd = ui->theme.border;
	unsigned short ot = ui->theme.text;
	int fired;

	if (active) {
		ui->theme.button = C_NAVY;
		ui->theme.button_hot = C_NAVY;
		ui->theme.button_down = C_NAVY;
		ui->theme.border = C_NAVY;
		ui->theme.text = C_TEXT_LIGHT;
	} else {
		ui->theme.button = C_MENU;
		ui->theme.button_hot = C_MENU_HOT;
		ui->theme.button_down = NGUI_RGB(160, 160, 160);
		ui->theme.border = C_MENU;
		ui->theme.text = C_TEXT;
	}
	fired = ngui_button(ui, id, r, label);
	ui->theme.button = ob;
	ui->theme.button_hot = oh;
	ui->theme.button_down = od;
	ui->theme.border = obd;
	ui->theme.text = ot;
	return fired;
}

static void draw_menu_bar(struct ngui *ui, const struct sol_layout *l)
{
	struct ngui_rect gr = rect_make(6, 2, 44, 18);
	struct ngui_rect hr = rect_make(52, 2, 44, 18);

	(void)l;
	ngui_fill_rect(ui, rect_make(0, 0, ui->w, MENU_H), C_MENU);
	ngui_fill_rect(ui, rect_make(0, 0, ui->w, 1), C_MENU_TOP);
	ngui_fill_rect(ui, rect_make(0, MENU_H - 1, ui->w, 1), C_MENU_DARK);

	if (menu_bar_button(ui, ID_MENU_GAME, gr, "Game",
			    menu_open == MENU_GAME)) {
		menu_open = (menu_open == MENU_GAME) ? MENU_NONE : MENU_GAME;
		ngui_invalidate(ui);
	}
	if (menu_bar_button(ui, ID_MENU_HELP, hr, "Help",
			    menu_open == MENU_HELP)) {
		menu_open = (menu_open == MENU_HELP) ? MENU_NONE : MENU_HELP;
		ngui_invalidate(ui);
	}
}

static void draw_dropdown(struct ngui *ui)
{
	const struct menu_entry *e;
	int n, i, x, y, w, h, ey;

	if (menu_open == MENU_NONE)
		return;
	n = menu_entries(menu_open, &e);
	x = menu_x(menu_open);
	y = MENU_H;
	w = menu_w(menu_open);
	h = menu_dropdown_h(menu_open);

	ngui_fill_rect(ui, rect_make(x + 2, y + 2, w, h),
		       NGUI_RGB(96, 96, 96));
	ngui_fill_rect(ui, rect_make(x, y, w, h), C_MENU);
	ngui_frame(ui, rect_make(x, y, w, h), C_MENU_DARK);
	ngui_fill_rect(ui, rect_make(x + 1, y + 1, w - 2, 1), C_MENU_TOP);

	ey = y + 3;
	for (i = 0; i < n; i++) {
		int eh = menu_entry_h(&e[i]);

		if (!e[i].label) {
			ngui_fill_rect(ui, rect_make(x + 4, ey + 3, w - 8, 1),
				       NGUI_RGB(128, 128, 128));
			ey += eh;
			continue;
		}
		if (ngui_menu_item(ui, ID_MENU_ITEM + (unsigned int)e[i].action,
				   rect_make(x + 2, ey, w - 4, eh),
				   e[i].label, e[i].shortcut,
				   action_checked(e[i].action))) {
			pending_action = e[i].action;
			menu_open = MENU_NONE;
			ngui_invalidate(ui);
		}
		ey += eh;
	}
}

static void draw_status(struct ngui *ui, const struct sol_layout *l)
{
	char b[32], t[24];
	int n, y = l->status_y + 2;

	ngui_fill_rect(ui, rect_make(0, l->status_y, ui->w,
				     ui->h - l->status_y), C_MENU);
	ngui_fill_rect(ui, rect_make(0, l->status_y, ui->w, 1), C_MENU_TOP);
	ngui_fill_rect(ui, rect_make(0, ui->h - 1, ui->w, 1), C_MENU_DARK);

	n = 0;
	b[n++] = 'S';
	b[n++] = 'c';
	b[n++] = 'o';
	b[n++] = 'r';
	b[n++] = 'e';
	b[n++] = ':';
	b[n++] = ' ';
	fmt_int(b + n, score);
	ngui_text(ui, 8, y, b, C_TEXT);

	n = 0;
	t[n++] = 'T';
	t[n++] = 'i';
	t[n++] = 'm';
	t[n++] = 'e';
	t[n++] = ':';
	t[n++] = ' ';
	fmt_int(t + n, elapsed_seconds(u_ticks_ms()));
	{
		int tw = (int)text_len(t) * 8;

		ngui_text(ui, ui->w - 8 - tw, y, t, C_TEXT);
	}

	if (won) {
		const char *msg = "You win!";
		int mw = (int)text_len(msg) * 8;
		int mx = (ui->w - mw) / 2;
		int left_w = (int)text_len(b) * 8 + 8;
		int right_w = (int)text_len(t) * 8 + 8;

		if (mx > left_w + 12 && mx + mw < ui->w - right_w - 12)
			ngui_text(ui, mx, y, msg, C_NAVY);
	}
}

static void help_geometry(struct ngui *ui, struct ngui_rect *box,
			  struct ngui_rect *btn)
{
	int w = 470, h = 300;

	if (w > ui->w - 24)
		w = ui->w - 24;
	if (w < 280)
		w = 280;
	if (h > ui->h - 40)
		h = ui->h - 40;
	if (h < 220)
		h = 220;
	box->x = (ui->w - w) / 2;
	box->y = (ui->h - h) / 2;
	box->w = w;
	box->h = h;
	btn->x = box->x + (w - 70) / 2;
	btn->y = box->y + h - 30;
	btn->w = 70;
	btn->h = 22;
}

static void draw_help(struct ngui *ui)
{
	struct ngui_rect box, btn;
	const char *title = help_open == HELP_ABOUT ?
			    "About Solitaire" : "How to play";
	int y, i;

	help_geometry(ui, &box, &btn);
	ngui_fill_rect(ui, rect_make(box.x + 3, box.y + 3, box.w, box.h),
		       NGUI_RGB(80, 80, 80));
	ngui_fill_rect(ui, box, C_MENU);
	ngui_frame(ui, box, C_TEXT);
	ngui_fill_rect(ui, rect_make(box.x + 1, box.y + 1, box.w - 2, 22),
		       C_NAVY);
	ngui_text(ui, box.x + 8, box.y + 4, title, C_TEXT_LIGHT);
	if (menu_bar_button(ui, ID_HELP_X,
			    rect_make(box.x + box.w - 18, box.y + 1, 17, 22),
			    "x", 1)) {
		help_open = HELP_NONE;
		ngui_invalidate(ui);
	}

	y = box.y + 38;
	if (help_open == HELP_ABOUT) {
		ngui_text_styled(ui, box.x + 18, y, "Solitaire", 9,
				 C_TEXT, 2, 1);
		y += 36;
		for (i = 0; i < (int)(sizeof(about_lines) /
				      sizeof(about_lines[0])); i++) {
			if (y + 16 > box.y + box.h - 40)
				break;
			ngui_text(ui, box.x + 18, y, about_lines[i], C_TEXT);
			y += 18;
		}
	} else {
		for (i = 0; i < (int)(sizeof(howto_lines) /
				      sizeof(howto_lines[0])); i++) {
			if (y + 16 > box.y + box.h - 40)
				break;
			ngui_text(ui, box.x + 18, y, howto_lines[i], C_TEXT);
			y += 18;
		}
	}

	if (ngui_button(ui, ID_HELP_CLOSE, btn, "Close")) {
		help_open = HELP_NONE;
		ngui_invalidate(ui);
	}
}

/* ------------------------------------------------------------------ */
/* interaction                                                        */

static void menu_action(int action, unsigned int now)
{
	switch (action) {
	case ACT_NEW:
		new_game(now);
		break;
	case ACT_DRAW1:
		draw_three = 0;
		break;
	case ACT_DRAW3:
		draw_three = 1;
		break;
	case ACT_BLUE:
		deck_style = 0;
		break;
	case ACT_RED:
		deck_style = 1;
		break;
	case ACT_HOWTO:
		help_open = HELP_HOWTO;
		break;
	case ACT_ABOUT:
		help_open = HELP_ABOUT;
		break;
	default:
		break;
	}
}

static void game_press(struct ngui *ui, const struct sol_layout *l,
		       unsigned int now, int *changed)
{
	struct hit h;
	struct sel s;
	struct ngui_rect sr;

	if (won) {
		if (bounce_active || bounce_show) {
			bounce_active = 0;
			bounce_show = 0;
			*changed = 1;
		}
		return;
	}

	h = hit_test(l, ui->press_x, ui->press_y);
	if (h.kind == HIT_STOCK) {
		stock_click(now);
		*changed = 1;
		return;
	}

	s = selection_from_hit(&h);
	if (select_sel.kind != SRC_NONE && hit_within_selection(&h, &select_sel))
		s = select_sel;

	press_sel = s;
	press_target = target_from_hit(&h);
	press_x = ui->press_x;
	press_y = ui->press_y;
	press_active = 1;

	sr = sel_rect(l, &s);
	press_card_x = sr.x;
	press_card_y = sr.y;
	if (s.kind == SRC_NONE) {
		press_card_x = ui->press_x;
		press_card_y = ui->press_y;
	}
}

static void select_click(const struct sol_layout *l, unsigned int now,
			 int *changed)
{
	int dbl = 0;

	if (last_click_valid && same_sel(&press_sel, &last_click_sel) &&
	    (unsigned int)(now - last_click_ms) < 800u)
		dbl = 1;

	if (dbl && press_sel.kind != SRC_NONE) {
		if (auto_to_foundation(&press_sel, now)) {
			*changed = 1;
			last_click_valid = 0;
			return;
		}
	}

	if (select_sel.kind != SRC_NONE && press_target.kind != TGT_NONE) {
		int same_source = (select_sel.kind == SRC_TABLEAU &&
				   press_target.kind == TGT_TABLEAU &&
				   select_sel.pile == press_target.pile) ||
				  (select_sel.kind == SRC_FOUNDATION &&
				   press_target.kind == TGT_FOUNDATION &&
				   select_sel.pile == press_target.pile);

		if (!same_source) {
			if (perform_move(&select_sel, &press_target, now)) {
				*changed = 1;
				last_click_valid = 0;
				return;
			}
		}
	}

	if (press_sel.kind != SRC_NONE) {
		if (same_sel(&press_sel, &select_sel)) {
			clear_selection();
			last_click_valid = 0;
		} else {
			select_sel = press_sel;
			last_click_sel = press_sel;
			last_click_ms = now;
			last_click_valid = 1;
		}
		*changed = 1;
		return;
	}

	if (press_target.kind == TGT_NONE) {
		if (select_sel.kind != SRC_NONE) {
			clear_selection();
			*changed = 1;
		}
		last_click_valid = 0;
	}
	(void)l;
}

static int ui_press_consumed(struct ngui *ui)
{
	if (help_open) {
		struct ngui_rect box, btn;

		help_geometry(ui, &box, &btn);
		if (!point_in(&box, ui->press_x, ui->press_y)) {
			help_open = HELP_NONE;
			ngui_invalidate(ui);
		}
		return 1;
	}
	if (ui->press_y < MENU_H) {
		if (menu_open != MENU_NONE &&
		    !(ui->press_x >= 6 && ui->press_x < 50) &&
		    !(ui->press_x >= 52 && ui->press_x < 96)) {
			menu_open = MENU_NONE;
			ngui_invalidate(ui);
		}
		return 1;
	}
	if (menu_open != MENU_NONE) {
		struct ngui_rect dr;
		int x = menu_x(menu_open);
		int y = MENU_H;
		int w = menu_w(menu_open);
		int h = menu_dropdown_h(menu_open);

		dr = rect_make(x, y, w, h);
		if (!point_in(&dr, ui->press_x, ui->press_y)) {
			menu_open = MENU_NONE;
			ngui_invalidate(ui);
		}
		return 1;
	}
	return 0;
}

static int handle_input(struct ngui *ui, const struct sol_layout *l,
			unsigned int now)
{
	int changed = 0;
	int k;

	while ((k = ngui_key(ui)) != 0) {
		if (k == NGUI_KEY_ESC) {
			if (help_open) {
				help_open = HELP_NONE;
				changed = 1;
			} else if (menu_open != MENU_NONE) {
				menu_open = MENU_NONE;
				changed = 1;
			} else {
				quit = 1;
			}
		} else if (k == 'n' || k == 'N' || k == 'r' || k == 'R') {
			new_game(now);
			changed = 1;
		} else if (k == 'h' || k == 'H') {
			help_open = HELP_HOWTO;
			menu_open = MENU_NONE;
			changed = 1;
		}
	}

	if (ui->pressed) {
		if (ui_press_consumed(ui)) {
			gesture_consumed = 1;
			changed = 1;
		} else {
			game_press(ui, l, now, &changed);
		}
	}

	if (press_active && !drag_active && !ui->released &&
	    press_sel.kind != SRC_NONE && (ui->buttons & 1u)) {
		int dx = ui->mouse_x - press_x;
		int dy = ui->mouse_y - press_y;

		if (dx < 0)
			dx = -dx;
		if (dy < 0)
			dy = -dy;
		if (dx > 4 || dy > 4) {
			drag_active = 1;
			drag_sel = press_sel;
			drag_dx = press_x - press_card_x;
			drag_dy = press_y - press_card_y;
			select_sel = press_sel;
			last_click_valid = 0;
			changed = 1;
		}
	}

	if (ui->released) {
		if (gesture_consumed) {
			gesture_consumed = 0;
		} else if (press_active) {
			if (drag_active) {
				struct target t;

				t = drop_target_at(l, ui->release_x,
						   ui->release_y);
				if (can_drop(&drag_sel, &t))
					perform_move(&drag_sel, &t, now);
				else
					clear_selection();
				drag_active = 0;
				press_active = 0;
				changed = 1;
			} else {
				select_click(l, now, &changed);
				press_active = 0;
			}
		}
	}
	return changed;
}

/* ------------------------------------------------------------------ */
/* win animation                                                      */

static int bounce_update(struct ngui *ui, const struct sol_layout *l,
			 unsigned int now)
{
	int i;
	int changed = 0;

	if (!bounce_active)
		return 0;

	if (bounce_count < BOUNCE_MAX) {
		unsigned int wait = bounce_count == 0 ? 0u : 105u;

		if ((unsigned int)(now - bounce_spawn_ms) >= wait) {
			struct bounce_card *b = &bounce_cards[bounce_count];
			int idx = (bounce_count * 7 + 3) % 48;

			b->active = 1;
			b->settled = 0;
			/* A unique, non-king card per bouncing card: the four
			 * visible foundation kings stay unique. */
			b->id = (idx / 12) * 13 + (idx % 12);
			b->x = l->col_x[3 + (int)(rng_next() % 4)];
			b->y = l->top_y;
			b->vx = (int)(rng_next() % 7) - 3;
			if (b->vx == 0)
				b->vx = 2;
			b->vy = -2 - (int)(rng_next() % 4);
			bounce_count++;
			bounce_spawn_ms = now;
			changed = 1;
		}
	}

	for (i = 0; i < BOUNCE_MAX; i++) {
		struct bounce_card *b = &bounce_cards[i];

		if (!b->active)
			continue;
		b->x += b->vx;
		b->y += b->vy;
		b->vy += 1;

		if (b->x < 0) {
			b->x = 0;
			b->vx = -b->vx;
		}
		if (b->x + l->cw > ui->w) {
			b->x = ui->w - l->cw;
			if (b->x < 0)
				b->x = 0;
			b->vx = -b->vx;
		}
		if (b->y < 0) {
			b->y = 0;
			if (b->vy < 0)
				b->vy = -b->vy;
		}
		if (b->y + l->ch > l->status_y - 2) {
			b->y = l->status_y - 2 - l->ch;
			if (b->y < 0)
				b->y = 0;
			if (b->vy > 0)
				b->vy = -(b->vy * 5 / 8);
			b->vx = b->vx * 7 / 8;
			if (b->vy > -2) {
				b->vy = 0;
				if (b->vx < 2 && b->vx > -2) {
					b->vx = 0;
					b->active = 0;
					b->settled = 1;
				}
			}
		}
		changed = 1;
	}

	if (bounce_count >= BOUNCE_MAX) {
		int any = 0;

		for (i = 0; i < BOUNCE_MAX; i++)
			if (bounce_cards[i].active)
				any = 1;
		if (!any)
			bounce_active = 0;
	} else {
		changed = 1;
	}

	if ((unsigned int)(now - bounce_start_ms) > 20000u) {
		for (i = 0; i < bounce_count; i++) {
			bounce_cards[i].active = 0;
			bounce_cards[i].settled = 1;
		}
		bounce_active = 0;
	}
	return changed;
}

/* ------------------------------------------------------------------ */
/* app                                                                */

static void draw_app(struct ngui *ui, const struct sol_layout *l)
{
	ngui_fill_rect(ui, rect_make(0, 0, ui->w, ui->h), C_FELT);

	draw_stock(ui, l);
	draw_waste(ui, l);
	draw_foundations(ui, l);
	draw_tableau(ui, l);

	draw_drop_highlight(ui, l);
	draw_selection_outline(ui, l);
	draw_bounce_cards(ui, l);
	if (drag_active)
		draw_drag_cards(ui, l);

	draw_menu_bar(ui, l);
	draw_status(ui, l);
	draw_dropdown(ui);
	if (help_open)
		draw_help(ui);
}

void _start(const char *arg)
{
	static const struct {
		int w, h;
	} sizes[] = {
		{ START_W, START_H },
		{ 700, 520 },
		{ 640, 460 },
		{ 600, 400 },
		{ 520, 380 },
		{ 440, 340 },
		{ 360, 300 }
	};
	struct win_info info;
	struct ngui ui;
	unsigned int now;
	unsigned int i;
	int created = 0;

	(void)arg;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (u_win_create((unsigned int)sizes[i].w,
				 (unsigned int)sizes[i].h,
				 "Solitaire", &info) == 0) {
			created = 1;
			break;
		}
	}
	if (!created) {
		u_puts("solitaire: no graphics window\n");
		u_exit(1);
	}

	ngui_init(&ui, (unsigned short *)info.fb, (int)info.width,
		  (int)info.height);
	setup_theme(&ui);
	draw_three = 0;
	deck_style = 0;
	quit = 0;
	now = u_ticks_ms();
	new_game(now);

	for (;;) {
		struct sol_layout l;
		int want, changed, sec;

		now = u_ticks_ms();
		want = ngui_begin(&ui);
		if (quit)
			break;
		changed = 0;
		if (pending_action != ACT_NONE) {
			int act = pending_action;

			pending_action = ACT_NONE;
			menu_action(act, now);
			changed = 1;
		}
		compute_layout(&ui, &l);
		changed |= handle_input(&ui, &l, now);
		if (bounce_active)
			changed |= bounce_update(&ui, &l, now);
		sec = elapsed_seconds(now);
		if (sec != last_sec) {
			last_sec = sec;
			changed = 1;
		}
		if (want || changed) {
			draw_app(&ui, &l);
			ngui_end(&ui);
		} else {
			ngui_end_ex(&ui, 0);
		}
		if (quit)
			break;
		u_sleep_ms(bounce_active ? 20 : 10);
	}
	u_win_close();
	u_exit(0);
}
