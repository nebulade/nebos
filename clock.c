/* clock command: analog clock in VGA mode 13h. Press ESC to return. */

#include "io.h"
#include "rtc.h"
#include "timer.h"
#include "vga.h"

#define CX          (VGA_GFX_WIDTH / 2)
#define CY          (VGA_GFX_HEIGHT / 2)
#define RADIUS      80
#define RAD_HOUR    40
#define RAD_MIN     56
#define RAD_SEC     72
#define TICK_LEN    6

#define COL_BG      0
#define COL_FACE    15   /* white */
#define COL_TICK    7    /* gray */
#define COL_HOUR    15
#define COL_MIN     15
#define COL_SEC     12   /* light red */
#define COL_CENTER  15

/*
 * Precomputed unit-vector table at radius 64 for 60 clock positions.
 * Index 0 = 12 o'clock (up), 15 = 3 o'clock, 30 = 6 o'clock, etc.
 * x = 64*sin(2*pi*i/60), y = -64*cos(2*pi*i/60)
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

/* ---- drawing primitives ---- */

static void draw_line(int x0, int y0, int x1, int y1, unsigned char color)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int ax = (dx < 0) ? -dx : dx;
	int ay = (dy < 0) ? -dy : dy;
	int steps = (ax > ay) ? ax : ay;
	int i;

	if (steps == 0) {
		vga_put_pixel(x0, y0, color);
		return;
	}
	for (i = 0; i <= steps; i++) {
		int x = x0 + dx * i / steps;
		int y = y0 + dy * i / steps;
		vga_put_pixel(x, y, color);
	}
}

static void draw_circle(int cx, int cy, int r, unsigned char color)
{
	int x = r, y = 0;
	int d = 1 - r;

	while (y <= x) {
		vga_put_pixel(cx + x, cy + y, color);
		vga_put_pixel(cx - x, cy + y, color);
		vga_put_pixel(cx + x, cy - y, color);
		vga_put_pixel(cx - x, cy - y, color);
		vga_put_pixel(cx + y, cy + x, color);
		vga_put_pixel(cx - y, cy + x, color);
		vga_put_pixel(cx + y, cy - x, color);
		vga_put_pixel(cx - y, cy - x, color);
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

/* ---- clock rendering ---- */

static void draw_face(void)
{
	int i;

	draw_circle(CX, CY, RADIUS, COL_FACE);
	draw_circle(CX, CY, RADIUS - 1, COL_FACE);

	/* Hour tick marks */
	for (i = 0; i < 60; i += 5) {
		int inner = RADIUS - TICK_LEN;
		int ox = CX + tab_x[i] * RADIUS / 64;
		int oy = CY + tab_y[i] * RADIUS / 64;
		int ix = CX + tab_x[i] * inner / 64;
		int iy = CY + tab_y[i] * inner / 64;
		draw_line(ix, iy, ox, oy, COL_TICK);
	}
}

static void draw_hand(int idx60, int length, unsigned char color, int thick)
{
	int ex = CX + tab_x[idx60] * length / 64;
	int ey = CY + tab_y[idx60] * length / 64;

	if (thick)
		draw_thick_line(CX, CY, ex, ey, color);
	else
		draw_line(CX, CY, ex, ey, color);
}

static void draw_hands(unsigned int h, unsigned int m, unsigned int s)
{
	int hour_idx;

	/* Hour hand: 5 positions per hour + minute offset */
	hour_idx = (int)((h % 12) * 5 + m / 12);
	if (hour_idx >= 60)
		hour_idx = 59;

	draw_hand(hour_idx, RAD_HOUR, COL_HOUR, 1);
	draw_hand((int)m, RAD_MIN, COL_MIN, 1);
	draw_hand((int)s, RAD_SEC, COL_SEC, 0);

	/* Center dot */
	vga_put_pixel(CX, CY, COL_CENTER);
	vga_put_pixel(CX + 1, CY, COL_CENTER);
	vga_put_pixel(CX, CY + 1, COL_CENTER);
	vga_put_pixel(CX + 1, CY + 1, COL_CENTER);
}

/* ---- main entry point ---- */

void program_clock_main(void)
{
	unsigned int last_sec = 99;
	unsigned int h, m, s;
	unsigned int tick;

	vga_set_mode_13();
	vga_clear(COL_BG);
	io_drain_keyboard();

	for (;;) {
		int k = io_poll_key();
		if (k == 0x1B)
			break;

		rtc_read_hms(&h, &m, &s);
		if (s != last_sec) {
			vga_clear(COL_BG);
			draw_face();
			draw_hands(h, m, s);
			last_sec = s;
		}

		tick = get_ticks_ms();
		while (get_ticks_ms() - tick < 50)
			__asm__ volatile ("hlt");
	}

	vga_set_mode_text();
	io_clear_screen();
}
