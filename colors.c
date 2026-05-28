/* colors command: full-screen animated rainbow in VESA LFB. Target 60fps. Press ESC to return. */

#include "fbtext.h"
#include "io.h"
#include "timer.h"
#include "vga.h"

#define RAINBOW_SEG   256
#define RAINBOW_FULL  (6 * RAINBOW_SEG)
#define FRAME_MS      16
#define PHASE_STEP    32
/* FPS overlay: 7 chars × 8 px wide, 8 px tall. Rainbow must not draw here. */
#define FPS_RECT_W    (7 * 8)
#define FPS_RECT_H    8

static void hue_to_rgb(unsigned int hue, unsigned int *r, unsigned int *g, unsigned int *b)
{
	unsigned int seg = hue / RAINBOW_SEG;
	unsigned int f   = hue % RAINBOW_SEG;
	unsigned int r1, g1, b1, r2, g2, b2;
	if (seg >= 6)
		seg = 0;
	switch (seg) {
		case 0: r1=255; g1=0;   b1=0;   r2=255; g2=255; b2=0;   break; /* R → Y */
		case 1: r1=255; g1=255; b1=0;   r2=0;   g2=255; b2=0;   break; /* Y → G */
		case 2: r1=0;   g1=255; b1=0;   r2=0;   g2=255; b2=255; break; /* G → C */
		case 3: r1=0;   g1=255; b1=255; r2=0;   g2=0;   b2=255; break; /* C → B */
		case 4: r1=0;   g1=0;   b1=255; r2=255; g2=0;   b2=255; break; /* B → M */
		default: r1=255; g1=0; b1=255; r2=255; g2=0;   b2=0;   break; /* M → R */
	}
	*r = (r1 * (RAINBOW_SEG - f) + r2 * f) / RAINBOW_SEG;
	*g = (g1 * (RAINBOW_SEG - f) + g2 * f) / RAINBOW_SEG;
	*b = (b1 * (RAINBOW_SEG - f) + b2 * f) / RAINBOW_SEG;
}

static void draw_frame(unsigned char *fb, unsigned int w, unsigned int h,
		unsigned int pitch, unsigned int bpp, unsigned int phase)
{
	/* One full rainbow across width and height (stretched) */
	unsigned int scale_x = RAINBOW_FULL / (w ? w : 1);
	unsigned int scale_y = RAINBOW_FULL / (h ? h : 1);
	unsigned int y, x;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			/* Leave FPS rect untouched so the overlay is never overwritten */
			if (y < FPS_RECT_H && x < FPS_RECT_W)
				continue;
			unsigned int hue = (phase + x * scale_x + y * scale_y) % RAINBOW_FULL;
			unsigned int r, g, b;
			hue_to_rgb(hue, &r, &g, &b);

			if (bpp == 16) {
				unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
				unsigned int off = (unsigned long)y * pitch + x * 2;
				fb[off]     = (unsigned char)(rgb16 & 0xFF);
				fb[off + 1] = (unsigned char)(rgb16 >> 8);
			} else if (bpp == 24) {
				unsigned int off = (unsigned long)y * pitch + x * 3;
				fb[off]     = (unsigned char)b;
				fb[off + 1] = (unsigned char)g;
				fb[off + 2] = (unsigned char)r;
			} else if (bpp == 32) {
				volatile unsigned int *row = (volatile unsigned int *)(fb + (unsigned long)y * pitch);
				row[x] = (unsigned int)b | ((unsigned int)g << 8) | ((unsigned int)r << 16);
			}
		}
	}
}

#define FPS_BUF_SIZE  8

static void format_fps(unsigned int fps, char *buf)
{
	unsigned int f = fps;
	if (f > 999)
		f = 999;
	buf[0] = (char)('0' + (f / 100));
	buf[1] = (char)('0' + ((f / 10) % 10));
	buf[2] = (char)('0' + (f % 10));
	buf[3] = ' ';
	buf[4] = 'f';
	buf[5] = 'p';
	buf[6] = 's';
	buf[7] = '\0';
}

void program_colors_main(void)
{
	unsigned char *fb;
	unsigned int w, h, pitch, bpp;
	unsigned int phase = 0;
	unsigned int frame_start;
	unsigned int frame_count = 0;
	unsigned int last_fps_ticks;
	unsigned int displayed_fps = 0;
	char fps_buf[FPS_BUF_SIZE];

	if (!vga_has_vesa())
		return;
	fb = (unsigned char *)vga_fb_ptr();
	if (!fb)
		return;
	w = vga_fb_width();
	h = vga_fb_height();
	pitch = vga_fb_pitch();
	bpp = vga_fb_bpp();
	if (bpp != 16 && bpp != 24 && bpp != 32)
		return;

	io_drain_keyboard();
	last_fps_ticks = get_ticks_ms();

	for (;;) {
		frame_start = get_ticks_ms();
		if (io_poll_key() == 0x1B)
			break;
		draw_frame(fb, w, h, pitch, bpp, phase);
		phase = (phase + PHASE_STEP) % RAINBOW_FULL;

		frame_count++;
		if (get_ticks_ms() - last_fps_ticks >= 1000) {
			displayed_fps = frame_count;
			frame_count = 0;
			last_fps_ticks = get_ticks_ms();
		}
		format_fps(displayed_fps, fps_buf);
		fbtext_draw_string_xy(0, 0, fps_buf, 255, 255, 255, 0);

		while (get_ticks_ms() - frame_start < FRAME_MS)
			__asm__ volatile ("hlt");
	}
}
