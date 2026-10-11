/* colors command: full-screen animated rainbow in VESA LFB. Target 30fps (matches flush rate). Press ESC to return. */

#include "fbtext.h"
#include "io.h"
#include "timer.h"
#include "vga.h"

#define RAINBOW_SEG   256
#define RAINBOW_FULL  (6 * RAINBOW_SEG)
#define FRAME_MS      33 /* matches 30Hz flush rate */
#define PHASE_STEP    32
/* FPS overlay: 7 chars × 8 px wide, 8 px tall. Rainbow must not draw here. */
#define FPS_RECT_W    (7 * 8)
#define FPS_RECT_H    8

/* Precomputed hue-to-RGB lookup table. Packed as 0x00BBGGRR for fast 32-bit load.
 * The hue cycle is RAINBOW_FULL entries, but the table is decimated by
 * LUT_STEP: a 4-hue step is ~1.5% of a color segment and invisible, and the
 * smaller table keeps the kernel image under the 0x42000 memory-bound. */
#define LUT_STEP      4
#define LUT_SIZE      (RAINBOW_FULL / LUT_STEP)

static unsigned int hue_lut[LUT_SIZE];

static void build_hue_lut(void)
{
	unsigned int i;
	for (i = 0; i < LUT_SIZE; i++) {
		unsigned int hue = i * LUT_STEP;
		unsigned int seg = hue / RAINBOW_SEG;
		unsigned int f   = hue % RAINBOW_SEG;
		unsigned int r1, g1, b1, r2, g2, b2;
		if (seg >= 6)
			seg = 0;
		switch (seg) {
			case 0: r1=255; g1=0;   b1=0;   r2=255; g2=255; b2=0;   break;
			case 1: r1=255; g1=255; b1=0;   r2=0;   g2=255; b2=0;   break;
			case 2: r1=0;   g1=255; b1=0;   r2=0;   g2=255; b2=255; break;
			case 3: r1=0;   g1=255; b1=255; r2=0;   g2=0;   b2=255; break;
			case 4: r1=0;   g1=0;   b1=255; r2=255; g2=0;   b2=255; break;
			default: r1=255; g1=0; b1=255; r2=255; g2=0;   b2=0;   break;
		}
		unsigned int r = (r1 * (RAINBOW_SEG - f) + r2 * f) / RAINBOW_SEG;
		unsigned int g = (g1 * (RAINBOW_SEG - f) + g2 * f) / RAINBOW_SEG;
		unsigned int b = (b1 * (RAINBOW_SEG - f) + b2 * f) / RAINBOW_SEG;
		hue_lut[i] = b | (g << 8) | (r << 16);
	}
}

static void draw_frame(unsigned char *fb, unsigned int w, unsigned int h,
		unsigned int pitch, unsigned int bpp, unsigned int phase)
{
	unsigned int scale_x = RAINBOW_FULL / (w ? w : 1);
	unsigned int scale_y = RAINBOW_FULL / (h ? h : 1);
	unsigned int y;

	for (y = 0; y < h; y++) {
		unsigned int row_hue = (phase + y * scale_y) % RAINBOW_FULL;
		unsigned char *row = fb + (unsigned long)y * pitch;

		if (y < FPS_RECT_H) {
			/* Skip FPS rect columns */
			unsigned int x;
			if (bpp == 32) {
				unsigned int *p = (unsigned int *)row;
				for (x = 0; x < w; x++) {
					if (x >= FPS_RECT_W) {
						unsigned int packed = hue_lut[row_hue / LUT_STEP];
						*p = packed;
					}
					p++;
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
			} else if (bpp == 24) {
				for (x = 0; x < w; x++) {
					if (x >= FPS_RECT_W) {
						unsigned int packed = hue_lut[row_hue / LUT_STEP];
						row[x * 3]     = (unsigned char)(packed & 0xFF);
						row[x * 3 + 1] = (unsigned char)((packed >> 8) & 0xFF);
						row[x * 3 + 2] = (unsigned char)((packed >> 16) & 0xFF);
					}
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
			} else if (bpp == 16) {
				for (x = 0; x < w; x++) {
					if (x >= FPS_RECT_W) {
						unsigned int packed = hue_lut[row_hue / LUT_STEP];
						unsigned int r = (packed >> 16) & 0xFF;
						unsigned int g = (packed >> 8) & 0xFF;
						unsigned int b = packed & 0xFF;
						unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
						unsigned short *p = (unsigned short *)(row + x * 2);
						*p = (unsigned short)rgb16;
					}
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
			}
		} else {
			/* Full row, no FPS rect check — tight inner loop */
			unsigned int x;
			if (bpp == 32) {
				unsigned int *p = (unsigned int *)row;
				for (x = 0; x < w; x++) {
					*p++ = hue_lut[row_hue / LUT_STEP];
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
			} else if (bpp == 24) {
				for (x = 0; x < w; x++) {
					unsigned int packed = hue_lut[row_hue / LUT_STEP];
					row[x * 3]     = (unsigned char)(packed & 0xFF);
					row[x * 3 + 1] = (unsigned char)((packed >> 8) & 0xFF);
					row[x * 3 + 2] = (unsigned char)((packed >> 16) & 0xFF);
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
			} else if (bpp == 16) {
				for (x = 0; x < w; x++) {
					unsigned int packed = hue_lut[row_hue / LUT_STEP];
					unsigned int r = (packed >> 16) & 0xFF;
					unsigned int g = (packed >> 8) & 0xFF;
					unsigned int b = packed & 0xFF;
					unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
					unsigned short *p = (unsigned short *)(row + x * 2);
					*p = (unsigned short)rgb16;
					row_hue += scale_x;
					if (row_hue >= RAINBOW_FULL)
						row_hue -= RAINBOW_FULL;
				}
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
	build_hue_lut();
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

		/* Mark all rows dirty since we drew the full screen */
		vga_fb_mark_dirty_rect(0, h);

		/* Trigger flush so the 30Hz timer sees dirty rows */
		vga_fb_flush_pending_check();

		frame_count++;
		if (get_ticks_ms() - last_fps_ticks >= 1000) {
			displayed_fps = frame_count;
			frame_count = 0;
			last_fps_ticks = get_ticks_ms();
		}
		format_fps(displayed_fps, fps_buf);
		fbtext_draw_string_xy(0, 0, fps_buf, 255, 255, 255, 0);

		/* Sleep with hlt to give the system breathing room */
		while (get_ticks_ms() - frame_start < FRAME_MS)
			__asm__ volatile ("hlt");
	}
}
