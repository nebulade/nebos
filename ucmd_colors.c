/* colors: animated rainbow in the terminal window's canvas.  ESC returns.
 * Target 30 fps, matching the WM flush rate. */

#include "ucmd_lib.h"

#define RAINBOW_SEG   256
#define RAINBOW_FULL  (6 * RAINBOW_SEG)
#define FRAME_MS      33
#define PHASE_STEP    32

/* Hue LUT decimated by 4 (see the old kernel colors.c): stores RGB565. */
#define LUT_STEP      4
#define LUT_SIZE      (RAINBOW_FULL / LUT_STEP)

/* FPS overlay: 7 chars x 8 px, top-left; the rainbow skips this rect. */
#define FPS_RECT_W    (7 * 8)
#define FPS_RECT_H    8

static unsigned short hue_lut[LUT_SIZE];

static unsigned short rgb565(unsigned int r, unsigned int g, unsigned int b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void build_hue_lut(void)
{
	unsigned int i;

	for (i = 0; i < LUT_SIZE; i++) {
		unsigned int hue = i * LUT_STEP;
		unsigned int seg = hue / RAINBOW_SEG;
		unsigned int f = hue % RAINBOW_SEG;
		unsigned int r1, g1, b1, r2, g2, b2, r, g, b;

		if (seg >= 6)
			seg = 0;
		switch (seg) {
		case 0: r1 = 255; g1 = 0;   b1 = 0;   r2 = 255; g2 = 255; b2 = 0;   break;
		case 1: r1 = 255; g1 = 255; b1 = 0;   r2 = 0;   g2 = 255; b2 = 0;   break;
		case 2: r1 = 0;   g1 = 255; b1 = 0;   r2 = 0;   g2 = 255; b2 = 255; break;
		case 3: r1 = 0;   g1 = 255; b1 = 255; r2 = 0;   g2 = 0;   b2 = 255; break;
		case 4: r1 = 0;   g1 = 0;   b1 = 255; r2 = 255; g2 = 0;   b2 = 255; break;
		default: r1 = 255; g1 = 0; b1 = 255;  r2 = 255; g2 = 0;   b2 = 0;   break;
		}
		r = (r1 * (RAINBOW_SEG - f) + r2 * f) / RAINBOW_SEG;
		g = (g1 * (RAINBOW_SEG - f) + g2 * f) / RAINBOW_SEG;
		b = (b1 * (RAINBOW_SEG - f) + b2 * f) / RAINBOW_SEG;
		hue_lut[i] = rgb565(r, g, b);
	}
}

static void draw_frame(unsigned short *px, unsigned int w, unsigned int h,
		       unsigned int phase)
{
	unsigned int scale_x = RAINBOW_FULL / (w ? w : 1);
	unsigned int scale_y = RAINBOW_FULL / (h ? h : 1);
	unsigned int y, x;

	for (y = 0; y < h; y++) {
		unsigned int row_hue = (phase + y * scale_y) % RAINBOW_FULL;
		unsigned short *row = px + y * w;

		for (x = 0; x < w; x++) {
			if (!(y < FPS_RECT_H && x < FPS_RECT_W))
				row[x] = hue_lut[row_hue / LUT_STEP];
			row_hue += scale_x;
			if (row_hue >= RAINBOW_FULL)
				row_hue -= RAINBOW_FULL;
		}
	}
}

/* Minimal 8x8 glyphs for the FPS overlay: "0123456789 fps" (subset of
 * font_8x8.c, to keep the ELF small). */
static const unsigned char glyphs[13][8] = {
	{ 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 }, /* 0 */
	{ 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 }, /* 1 */
	{ 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 }, /* 2 */
	{ 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 }, /* 3 */
	{ 0x06,0x0E,0x1E,0x36,0x66,0x7F,0x06,0x00 }, /* 4 */
	{ 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 }, /* 5 */
	{ 0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00 }, /* 6 */
	{ 0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00 }, /* 7 */
	{ 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 }, /* 8 */
	{ 0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00 }, /* 9 */
	{ 0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00 }, /* f */
	{ 0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60 }, /* p */
	{ 0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00 }, /* s */
};

static int glyph_index(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c == 'f')
		return 10;
	if (c == 'p')
		return 11;
	if (c == 's')
		return 12;
	return -1;
}

static void draw_string(unsigned short *px, unsigned int w, unsigned int h,
			unsigned int x0, unsigned int y0, const char *s)
{
	for (; *s; s++, x0 += 8) {
		int gi = glyph_index(*s);
		unsigned int x, y;

		if (gi < 0)
			continue;      /* spaces etc. */
		for (y = 0; y < 8; y++) {
			if (y0 + y >= h)
				break;
			for (x = 0; x < 8; x++)
				if ((glyphs[gi][y] & (0x80u >> x)) && x0 + x < w)
					px[(y0 + y) * w + x0 + x] = rgb565(255, 255, 255);
		}
	}
}

static void format_fps(unsigned int fps, char *buf)
{
	if (fps > 999)
		fps = 999;
	buf[0] = (char)('0' + fps / 100);
	buf[1] = (char)('0' + (fps / 10) % 10);
	buf[2] = (char)('0' + fps % 10);
	buf[3] = ' ';
	buf[4] = 'f';
	buf[5] = 'p';
	buf[6] = 's';
	buf[7] = '\0';
}

void _start(const char *arg)
{
	struct sys_gfx_info info;
	unsigned short *px;
	unsigned int w, h, phase = 0, frames = 0, last_fps, displayed = 0;
	char fps_buf[8];

	(void)arg;
	if (u_gfx_begin(&info) != 0) {
		u_puts("colors: no graphics window\n");
		u_exit(1);
	}
	px = (unsigned short *)info.fb;
	w = info.width;
	h = info.height;
	build_hue_lut();
	last_fps = u_ticks_ms();

	for (;;) {
		unsigned int frame_start, elapsed;

		if (u_key_poll() == 0x1B)
			break;
		frame_start = u_ticks_ms();

		draw_frame(px, w, h, phase);
		phase = (phase + PHASE_STEP) % RAINBOW_FULL;

		frames++;
		if (frame_start - last_fps >= 1000) {
			displayed = frames;
			frames = 0;
			last_fps = frame_start;
		}
		format_fps(displayed, fps_buf);
		draw_string(px, w, h, 0, 0, fps_buf);
		u_gfx_present();

		elapsed = u_ticks_ms() - frame_start;
		if (elapsed < FRAME_MS)
			u_sleep_ms(FRAME_MS - elapsed);
	}
	u_gfx_end();
	u_exit(0);
}
