/* clock: analog clock drawn into the terminal window's canvas.  ESC (or
 * Ctrl+C, which the terminal turns into a kill) returns to the shell. */

#include "ucmd_lib.h"

/* Palette index to RGB. 0=black, 7=gray, 12=light red, 15=white */
static const unsigned char pal_r[16] = { 0, 0, 0, 0, 170, 170, 170, 170, 85, 85, 85, 85, 255, 255, 255, 255 };
static const unsigned char pal_g[16] = { 0, 0, 170, 170, 0, 0, 85, 170, 85, 85, 255, 255, 85, 85, 255, 255 };
static const unsigned char pal_b[16] = { 0, 170, 0, 170, 0, 170, 0, 170, 85, 255, 85, 255, 85, 255, 85, 255 };

#define COL_BG      0
#define COL_FACE    15
#define COL_TICK    7
#define COL_HOUR    15
#define COL_MIN     15
#define COL_SEC     12
#define COL_CENTER  15

/* Unit-vector table at radius 64 for 60 clock positions (from the old kernel
 * clock.c). */
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

/* Hour tick positions at radius 256; 256*sin(h*30), -256*cos(h*30). */
static const short hour_tab_x[12] = {
	   0,  128,  222,  256,  222,  128,    0, -128, -222, -256, -222, -128
};
static const short hour_tab_y[12] = {
	-256, -222, -128,    0,  128,  222,  256,  222,  128,    0, -128, -222
};

static unsigned short *fb;
static unsigned int cw, ch;
static unsigned int cx, cy, radius, rad_hour, rad_min, rad_sec, tick_len;

static void clock_geometry(void)
{
	cx = cw / 2;
	cy = ch / 2;
	radius = (cw < ch) ? cw / 2 : ch / 2;
	if (radius > 200)
		radius = 200;
	rad_hour = radius / 2;
	rad_min = radius * 7 / 10;
	rad_sec = radius * 9 / 10;
	tick_len = radius / 13;
	if (tick_len < 2)
		tick_len = 2;
}

static unsigned short rgb565(unsigned int r, unsigned int g, unsigned int b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void put_pixel(int x, int y, unsigned char color)
{
	if (x < 0 || y < 0 || (unsigned int)x >= cw || (unsigned int)y >= ch)
		return;
	fb[(unsigned int)y * cw + (unsigned int)x] =
		rgb565(pal_r[color], pal_g[color], pal_b[color]);
}

static void clear_screen(unsigned char color)
{
	unsigned int i, n = cw * ch;
	unsigned short v = rgb565(pal_r[color], pal_g[color], pal_b[color]);

	for (i = 0; i < n; i++)
		fb[i] = v;
}

static void draw_line(int x0, int y0, int x1, int y1, unsigned char color)
{
	int dx = x1 - x0, dy = y1 - y0;
	int ax = (dx < 0) ? -dx : dx, ay = (dy < 0) ? -dy : dy;
	int steps = (ax > ay) ? ax : ay;
	int i;

	if (steps == 0) {
		put_pixel(x0, y0, color);
		return;
	}
	for (i = 0; i <= steps; i++)
		put_pixel(x0 + dx * i / steps, y0 + dy * i / steps, color);
}

static void draw_circle(int ccx, int ccy, int r, unsigned char color)
{
	int x = r, y = 0, d = 1 - r;

	while (y <= x) {
		put_pixel(ccx + x, ccy + y, color);
		put_pixel(ccx - x, ccy + y, color);
		put_pixel(ccx + x, ccy - y, color);
		put_pixel(ccx - x, ccy - y, color);
		put_pixel(ccx + y, ccy + x, color);
		put_pixel(ccx - y, ccy + x, color);
		put_pixel(ccx + y, ccy - x, color);
		put_pixel(ccx - y, ccy - x, color);
		if (d <= 0)
			d += 2 * y + 3;
		else {
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
	int i, inner = (int)radius - (int)tick_len;

	draw_circle((int)cx, (int)cy, (int)radius, COL_FACE);
	draw_circle((int)cx, (int)cy, (int)radius - 1, COL_FACE);
	for (i = 0; i < 12; i++) {
		int ox = (int)cx + (hour_tab_x[i] * (int)radius + 128) / 256;
		int oy = (int)cy + (hour_tab_y[i] * (int)radius + 128) / 256;
		int ix = (int)cx + (hour_tab_x[i] * inner + 128) / 256;
		int iy = (int)cy + (hour_tab_y[i] * inner + 128) / 256;
		draw_line(ix, iy, ox, oy, COL_TICK);
	}
}

static void draw_hand(int idx60, int length, unsigned char color, int thick)
{
	int ex = (int)cx + tab_x[idx60] * length / 64;
	int ey = (int)cy + tab_y[idx60] * length / 64;

	if (thick)
		draw_thick_line((int)cx, (int)cy, ex, ey, color);
	else
		draw_line((int)cx, (int)cy, ex, ey, color);
}

static void draw_hands(unsigned int h, unsigned int m, unsigned int s)
{
	int hour_idx = (int)((h % 12) * 5 + m / 12);

	if (hour_idx >= 60)
		hour_idx = 59;
	draw_hand(hour_idx, (int)rad_hour, COL_HOUR, 1);
	draw_hand((int)m, (int)rad_min, COL_MIN, 1);
	draw_hand((int)s, (int)rad_sec, COL_SEC, 0);
	put_pixel((int)cx, (int)cy, COL_CENTER);
	put_pixel((int)cx + 1, (int)cy, COL_CENTER);
	put_pixel((int)cx, (int)cy + 1, COL_CENTER);
	put_pixel((int)cx + 1, (int)cy + 1, COL_CENTER);
}

void _start(const char *arg)
{
	struct win_info info;
	struct sys_time tm;
	unsigned int last_sec = 99;

	(void)arg;
	if (u_win_create(512, 320, "clock", &info) != 0) {
		u_puts("clock: no graphics window\n");
		u_exit(1);
	}
	fb = (unsigned short *)info.fb;
	cw = info.width;
	ch = info.height;

	clock_geometry();

	for (;;) {
		struct win_info cur;
		int k = u_key_poll();

		if (u_win_info(&cur) == 0 &&
		    (cur.width != cw || cur.height != ch)) {
			if (u_win_ack_resize() == 0) {
				fb = (unsigned short *)cur.fb;
				cw = cur.width;
				ch = cur.height;
				clock_geometry();
				last_sec = 99;
			}
		}

		if (k == 0x1B || k == 0x03)
			break;
		u_rtc_hms(&tm);
		if (tm.hour > 23 || tm.minute > 59 || tm.second > 59) {
			/* bogus read (should not happen): never index with it */
			u_sleep_ms(50);
			continue;
		}
		if (tm.second != last_sec) {
			clear_screen(COL_BG);
			draw_face();
			draw_hands(tm.hour, tm.minute, tm.second);
			last_sec = tm.second;
			u_win_present();
		}
		u_sleep_ms(50);
	}
	u_win_close();
	u_exit(0);
}
