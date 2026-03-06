/* clock command: analog clock in VESA LFB (or mode 13h fallback). Press ESC to return. */

#include "io.h"
#include "rtc.h"
#include "timer.h"
#include "vga.h"

/* VGA palette index to RGB (for LFB). 0=black, 7=gray, 12=light red, 15=white */
static const unsigned int pal_r[16] = { 0, 0, 0, 0, 170, 170, 170, 170, 85, 85, 85, 85, 255, 255, 255, 255 };
static const unsigned int pal_g[16] = { 0, 0, 170, 170, 0, 0, 85, 170, 85, 85, 255, 255, 85, 85, 255, 255 };
static const unsigned int pal_b[16] = { 0, 170, 0, 170, 0, 170, 0, 170, 85, 255, 85, 255, 85, 255, 85, 255 };

#define COL_BG      0
#define COL_FACE    15
#define COL_TICK    7
#define COL_HOUR    15
#define COL_MIN     15
#define COL_SEC     12
#define COL_CENTER  15

/*
 * Precomputed unit-vector table at radius 64 for 60 clock positions.
 */
static const signed char tab_x[60] = {
	  0,  7, 13, 20, 26, 32, 37, 42, 47, 51,
	 55, 59, 62, 63, 64, 63, 62, 59, 55, 51,
	 47, 42, 37, 32, 26, 20, 13,  7,  0, -7,
	-13,-20,-26,-32,-37,-42,-47,-51,-55,-59,
	-62,-63,-64,-63,-62,-59,-55,-51,-47,-42,
	-37,-32,-26,-20,-13, -7
};
static const signed char tab_y[60] = {
	-64,-63,-62,-59,-55,-51,-47,-42,-37,-32,
	-26,-20,-13, -7,  0,  7, 13, 20, 26, 32,
	 37, 42, 47, 51, 55, 59, 62, 63, 64, 63,
	 62, 59, 55, 51, 47, 42, 37, 32, 26, 20,
	 13,  7,  0, -7,-13,-20,-26,-32,-37,-42,
	-47,-51,-55,-59,-62,-63
};

static int clock_use_vesa;
static unsigned int clock_cx, clock_cy, clock_radius, clock_rad_hour, clock_rad_min, clock_rad_sec, clock_tick_len;

static void put_pixel(int x, int y, unsigned char color)
{
	if (clock_use_vesa)
		vga_put_pixel_rgb((unsigned int)x, (unsigned int)y,
			pal_r[color], pal_g[color], pal_b[color]);
	else
		vga_put_pixel(x, y, color);
}

static void clear_screen(unsigned char color)
{
	if (clock_use_vesa)
		vga_clear_rgb(pal_r[color], pal_g[color], pal_b[color]);
	else
		vga_clear(color);
}

static void draw_line(int x0, int y0, int x1, int y1, unsigned char color)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int ax = (dx < 0) ? -dx : dx;
	int ay = (dy < 0) ? -dy : dy;
	int steps = (ax > ay) ? ax : ay;
	int i;

	if (steps == 0) {
		put_pixel(x0, y0, color);
		return;
	}
	for (i = 0; i <= steps; i++) {
		int x = x0 + dx * i / steps;
		int y = y0 + dy * i / steps;
		put_pixel(x, y, color);
	}
}

static void draw_circle(int cx, int cy, int r, unsigned char color)
{
	int x = r, y = 0;
	int d = 1 - r;

	while (y <= x) {
		put_pixel(cx + x, cy + y, color);
		put_pixel(cx - x, cy + y, color);
		put_pixel(cx + x, cy - y, color);
		put_pixel(cx - x, cy - y, color);
		put_pixel(cx + y, cy + x, color);
		put_pixel(cx - y, cy + x, color);
		put_pixel(cx + y, cy - x, color);
		put_pixel(cx - y, cy - x, color);
		if (d <= 0) {
			d += 2 * y + 3;
		} else {
			d += 2 * (y - x) + 5;
			x--;
		}
		y++;
	}
}

static void draw_thick_line(int x0, int y0, int x1, int y1, unsigned char color)
{
	draw_line(x0, y0, x1, y1, color);
	draw_line(x0 + 1, y0, x1 + 1, y1, color);
	draw_line(x0, y0 + 1, x1, y1 + 1, color);
}

static void draw_face(void)
{
	int i;
	int inner = clock_radius - clock_tick_len;

	draw_circle(clock_cx, clock_cy, clock_radius, COL_FACE);
	draw_circle(clock_cx, clock_cy, clock_radius - 1, COL_FACE);

	for (i = 0; i < 60; i += 5) {
		int ox = clock_cx + tab_x[i] * (int)clock_radius / 64;
		int oy = clock_cy + tab_y[i] * (int)clock_radius / 64;
		int ix = clock_cx + tab_x[i] * (int)inner / 64;
		int iy = clock_cy + tab_y[i] * (int)inner / 64;
		draw_line(ix, iy, ox, oy, COL_TICK);
	}
}

static void draw_hand(int idx60, int length, unsigned char color, int thick)
{
	int ex = clock_cx + tab_x[idx60] * (int)length / 64;
	int ey = clock_cy + tab_y[idx60] * (int)length / 64;

	if (thick)
		draw_thick_line(clock_cx, clock_cy, ex, ey, color);
	else
		draw_line(clock_cx, clock_cy, ex, ey, color);
}

static void draw_hands(unsigned int h, unsigned int m, unsigned int s)
{
	int hour_idx;

	hour_idx = (int)((h % 12) * 5 + m / 12);
	if (hour_idx >= 60)
		hour_idx = 59;

	draw_hand(hour_idx, (int)clock_rad_hour, COL_HOUR, 1);
	draw_hand((int)m, (int)clock_rad_min, COL_MIN, 1);
	draw_hand((int)s, (int)clock_rad_sec, COL_SEC, 0);

	put_pixel(clock_cx, clock_cy, COL_CENTER);
	put_pixel(clock_cx + 1, clock_cy, COL_CENTER);
	put_pixel(clock_cx, clock_cy + 1, COL_CENTER);
	put_pixel(clock_cx + 1, clock_cy + 1, COL_CENTER);
}

void program_clock_main(void)
{
	unsigned int last_sec = 99;
	unsigned int h, m, s;
	unsigned int tick;
	unsigned int w, h_;

	clock_use_vesa = vga_has_vesa();
	if (clock_use_vesa) {
		w = vga_fb_width();
		h_ = vga_fb_height();
		clock_cx = w / 2;
		clock_cy = h_ / 2;
		clock_radius = (w < h_) ? (w / 2) : (h_ / 2);
		if (clock_radius > 200)
			clock_radius = 200;
		clock_rad_hour = clock_radius / 2;
		clock_rad_min = clock_radius * 7 / 10;
		clock_rad_sec = clock_radius * 9 / 10;
		clock_tick_len = clock_radius / 13;
		if (clock_tick_len < 2)
			clock_tick_len = 2;
	} else {
		vga_set_mode_13();
		clock_cx = 160;
		clock_cy = 100;
		clock_radius = 80;
		clock_rad_hour = 40;
		clock_rad_min = 56;
		clock_rad_sec = 72;
		clock_tick_len = 6;
	}

	clear_screen(COL_BG);
	io_drain_keyboard();

	for (;;) {
		int k = io_poll_key();
		if (k == 0x1B)
			break;

		rtc_read_hms(&h, &m, &s);
		if (s != last_sec) {
			clear_screen(COL_BG);
			draw_face();
			draw_hands(h, m, s);
			last_sec = s;
		}

		tick = get_ticks_ms();
		while (get_ticks_ms() - tick < 50)
			__asm__ volatile ("hlt");
	}

	if (!clock_use_vesa) {
		vga_set_mode_text();
		io_clear_screen();
	}
}
