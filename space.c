/* Simple Space Invaders: pixel graphics, keyboard input. Uses kernel API at PROGRAM_API_ADDR. */

#include "run_api.h"

#define INV_ROWS      5
#define INV_COLS      10
#define INV_W         10
#define INV_H         8
#define SHIP_W        16
#define SHIP_H        8
#define BULLET_W      2
#define BULLET_H      6
#define MAX_BULLETS   4
#define FRAME_MS      33
#define INV_STEP      4
#define BULLET_SPEED  6
#define SHIP_SPEED    6

static const struct prog_api *const api = (const struct prog_api *)PROGRAM_API_ADDR;

/* Row colors: red, green, yellow, cyan, magenta */
static const unsigned int row_r[INV_ROWS] = { 255, 0, 255, 0, 255 };
static const unsigned int row_g[INV_ROWS] = { 0, 255, 255, 255, 0 };
static const unsigned int row_b[INV_ROWS] = { 0, 0, 0, 255, 255 };

static unsigned int fb_w, fb_h;
static int ship_x;
static int inv_x, inv_y;
static int inv_dir;
static unsigned char alive[INV_ROWS][INV_COLS];
static int num_alive;

static int bullet_x[MAX_BULLETS];
static int bullet_y[MAX_BULLETS];
static int bullet_active[MAX_BULLETS];
static int num_bullets;

static void put_rect(int x0, int y0, int w, int h, unsigned int r, unsigned int g, unsigned int b)
{
	int x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			api->put_pixel_rgb((unsigned int)(x0 + x), (unsigned int)(y0 + y), r, g, b);
}

static void draw_ship(void)
{
	int x = ship_x;
	int y = (int)fb_h - SHIP_H - 4;
	put_rect(x, y, SHIP_W, SHIP_H, 0, 255, 128);
}

static void draw_invader(int row, int col)
{
	int x = inv_x + col * (INV_W + 4);
	int y = inv_y + row * (INV_H + 2);
	if (!alive[row][col])
		return;
	put_rect(x, y, INV_W, INV_H, row_r[row], row_g[row], row_b[row]);
}

static void draw_bullets(void)
{
	int i;
	for (i = 0; i < MAX_BULLETS; i++)
		if (bullet_active[i])
			put_rect(bullet_x[i], bullet_y[i], BULLET_W, BULLET_H, 255, 255, 255);
}

static void spawn_bullet(void)
{
	int i;
	for (i = 0; i < MAX_BULLETS; i++) {
		if (!bullet_active[i]) {
			bullet_x[i] = ship_x + SHIP_W / 2 - BULLET_W / 2;
			bullet_y[i] = (int)fb_h - SHIP_H - 4 - BULLET_H;
			bullet_active[i] = 1;
			num_bullets++;
			break;
		}
	}
}

static void update_bullets(void)
{
	int i, r, c;
	int bx, by, ix, iy;

	for (i = 0; i < MAX_BULLETS; i++) {
		if (!bullet_active[i])
			continue;
		bullet_y[i] -= BULLET_SPEED;
		if (bullet_y[i] < 0) {
			bullet_active[i] = 0;
			num_bullets--;
			continue;
		}
		bx = bullet_x[i];
		by = bullet_y[i];
		for (r = 0; r < INV_ROWS; r++) {
			for (c = 0; c < INV_COLS; c++) {
				if (!alive[r][c])
					continue;
				ix = inv_x + c * (INV_W + 4);
				iy = inv_y + r * (INV_H + 2);
				if (bx + BULLET_W > ix && bx < ix + INV_W &&
				    by + BULLET_H > iy && by < iy + INV_H) {
					alive[r][c] = 0;
					num_alive--;
					bullet_active[i] = 0;
					num_bullets--;
					goto next_bullet;
				}
			}
		}
next_bullet:;
	}
}

static void update_invaders(void)
{
	int edge;

	inv_x += inv_dir * INV_STEP;
	edge = inv_x + INV_COLS * (INV_W + 4) - 4;
	if (inv_dir > 0 && edge >= (int)fb_w) {
		inv_dir = -1;
		inv_y += INV_H + 4;
		inv_x = (int)fb_w - (INV_COLS * (INV_W + 4) - 4);
		if (inv_x < 0)
			inv_x = 0;
	} else if (inv_dir < 0 && inv_x <= 0) {
		inv_dir = 1;
		inv_y += INV_H + 4;
		inv_x = 0;
	}
}

static void game_init(void)
{
	int r, c;
	fb_w = api->get_fb_width();
	fb_h = api->get_fb_height();
	if (fb_w < 100)
		fb_w = 320;
	if (fb_h < 100)
		fb_h = 200;

	ship_x = (int)fb_w / 2 - SHIP_W / 2;
	if (ship_x < 0)
		ship_x = 0;

	inv_x = 20;
	inv_y = 20;
	inv_dir = 1;
	num_alive = INV_ROWS * INV_COLS;
	for (r = 0; r < INV_ROWS; r++)
		for (c = 0; c < INV_COLS; c++)
			alive[r][c] = 1;

	num_bullets = 0;
	for (c = 0; c < MAX_BULLETS; c++)
		bullet_active[c] = 0;
}

void _start(void)
{
	unsigned int last_tick;
	int k;
	int r, c;

	game_init();
	last_tick = api->get_ticks_ms();

	for (;;) {
		int want_left = 0, want_right = 0, want_fire = 0;
		while (api->get_ticks_ms() - last_tick < FRAME_MS) {
			k = api->poll_key();
			if (k == IO_KEY_ESC)
				return;
			if (k == IO_KEY_LEFT)
				want_left = 1;
			if (k == IO_KEY_RIGHT)
				want_right = 1;
			if (k == IO_KEY_SPACE)
				want_fire = 1;
		}
		last_tick = api->get_ticks_ms();
		if (want_left && ship_x > 0)
			ship_x -= SHIP_SPEED;
		if (want_right && ship_x + SHIP_W < (int)fb_w)
			ship_x += SHIP_SPEED;
		if (want_fire)
			spawn_bullet();

		update_bullets();
		update_invaders();

		api->clear_rgb(8, 8, 32);
		while ((k = api->poll_key()) != 0) {
			if (k == IO_KEY_ESC)
				return;
			if (k == IO_KEY_LEFT)
				want_left = 1;
			if (k == IO_KEY_RIGHT)
				want_right = 1;
			if (k == IO_KEY_SPACE)
				want_fire = 1;
		}
		draw_ship();
		for (r = 0; r < INV_ROWS; r++) {
			for (c = 0; c < INV_COLS; c++)
				draw_invader(r, c);
			while ((k = api->poll_key()) != 0) {
				if (k == IO_KEY_ESC)
					return;
				if (k == IO_KEY_LEFT)
					want_left = 1;
				if (k == IO_KEY_RIGHT)
					want_right = 1;
				if (k == IO_KEY_SPACE)
					want_fire = 1;
			}
		}
		draw_bullets();

		if (num_alive <= 0)
			return;
		if (inv_y + INV_ROWS * (INV_H + 2) >= (int)fb_h - SHIP_H - 8)
			return;
	}
}
