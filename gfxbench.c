/* gfxbench: time the software drawing paths, so each rendering step can be
 * measured on target instead of guessed.
 *
 * The fill/copy/cell/canvas cases run against a screen-sized RAM layer, which
 * is what the window manager draws into (the shadow buffer), and the flush
 * case times the real shadow -> VRAM copy.  The screen itself is never
 * disturbed: no drawing lands in the shadow, so the flush copies the WM's
 * unchanged content. */

#include "blit.h"
#include "cpu.h"
#include "fbtext.h"
#include "gfx.h"
#include "io.h"
#include "mm.h"
#include "timer.h"
#include "vga.h"

#define LAYER_W 1280
#define LAYER_H 1024
#define CONTENT_W 514
#define CONTENT_H 320
#define WIN_W 514
#define WIN_H 338
#define CELLS_COLS 64
#define CELLS_ROWS 32

static void put_dec(unsigned int n)
{
	char buf[11];
	unsigned int i = sizeof(buf);

	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i) {
		buf[--i] = (char)('0' + n % 10);
		n /= 10;
	}
	while (i < sizeof(buf))
		io_putchar(buf[i++]);
}

static void bench(const char *name, unsigned int n, unsigned int bytes,
		  unsigned int ms)
{
	io_putstr(name);
	io_putstr("  ");
	put_dec(n);
	io_putstr("x ");
	put_dec(ms);
	io_putstr(" ms");
	if (ms) {
		/* 32-bit friendly MB/s: bytes/1024 is small enough to scale. */
		unsigned int kbs = (bytes / 1024) * n;
		unsigned int mbs = (kbs * 1000 / ms) / 1024;

		io_putstr("  ");
		put_dec(mbs);
		io_putstr(" MB/s");
	}
	io_putstr("\n");
}

static unsigned char *alloc_pages(unsigned int bytes)
{
	unsigned int pages = (bytes + 4095) / 4096;

	return (unsigned char *)mm_alloc_pages(pages);
}

static void free_pages(unsigned char *p, unsigned int bytes)
{
	unsigned int pages = (bytes + 4095) / 4096;
	unsigned int i;

	for (i = 0; i < pages; i++)
		mm_free_page((unsigned int)p + i * 4096);
}

void program_gfxbench_main(void)
{
	struct surface layer;
	struct gfx_canvas canvas;
	unsigned short *cells;
	unsigned char *layer_px, *cell_px;
	unsigned int t0, t1, i;
	unsigned int layer_bytes = LAYER_W * LAYER_H * 4;

	io_putstr("gfxbench (sse2: ");
	io_putstr(cpu_has_sse2() ? "yes" : "no");
	io_putstr(", lfb wc: ");
	io_putstr(vga_fb_lfb_wc() ? "yes" : "no");
	io_putstr(")\n");

	layer_px = alloc_pages(layer_bytes);
	if (!layer_px) {
		io_putstr("gfxbench: out of memory\n");
		return;
	}
	layer.px = layer_px;
	layer.w = LAYER_W;
	layer.h = LAYER_H;
	layer.pitch = LAYER_W * 4;
	layer.bpp = 32;
	layer.alpha = 255;

	vga_set_target(&layer);

	/* Full-screen fill: what a full redraw costs. */
	t0 = get_ticks_ms();
	for (i = 0; i < 10; i++)
		blit->fill(0, 0, LAYER_W, LAYER_H, 0x203040);
	t1 = get_ticks_ms();
	bench("fill screen ", 10, layer_bytes, t1 - t0);

	/* Window content fill (the per-cell background). */
	t0 = get_ticks_ms();
	for (i = 0; i < 100; i++)
		blit->fill(0, 0, CONTENT_W, CONTENT_H, 0x000000);
	t1 = get_ticks_ms();
	bench("fill content", 100, CONTENT_W * CONTENT_H * 4, t1 - t0);

	/* Window move: an overlapping rect copy, as a drag step does. */
	t0 = get_ticks_ms();
	for (i = 0; i < 100; i++)
		blit->copy(1, 1, 0, 0, WIN_W, WIN_H);
	t1 = get_ticks_ms();
	bench("copy window ", 100, WIN_W * WIN_H * 4, t1 - t0);

	/* Cell grid: a whole terminal content repaint (glyph blitting). */
	cell_px = alloc_pages(CELLS_COLS * CELLS_ROWS * 2);
	if (cell_px) {
		cells = (unsigned short *)cell_px;
		for (i = 0; i < CELLS_COLS * CELLS_ROWS; i++)
			cells[i] = (unsigned short)(0x0A00 | ('A' + (i % 26)));
		t0 = get_ticks_ms();
		for (i = 0; i < 50; i++)
			fbtext_draw_cells_clip(cells, CELLS_COLS, CELLS_ROWS,
				0, 0, 0, 0, CELLS_COLS * 8, CELLS_ROWS * 10);
		t1 = get_ticks_ms();
		bench("cells 64x32 ", 50, CELLS_COLS * CELLS_ROWS * 80 * 4, t1 - t0);
		free_pages(cell_px, CELLS_COLS * CELLS_ROWS * 2);
	}

	/* Canvas presentation: RGB565 -> 32 bpp expansion. */
	if (gfx_alloc(&canvas, CONTENT_W, CONTENT_H) == 0) {
		for (i = 0; i < CONTENT_W * CONTENT_H; i++)
			canvas.px[i] = (unsigned short)(i * 7);
		t0 = get_ticks_ms();
		for (i = 0; i < 20; i++)
			blit->canvas(&canvas, 0, 0, 0, 0, CONTENT_W, CONTENT_H);
		t1 = get_ticks_ms();
		bench("canvas 565  ", 20, CONTENT_W * CONTENT_H * 4, t1 - t0);
		gfx_free(&canvas);
	}

	vga_set_target(0);

	/* Flush: shadow -> VRAM.  This is the copy the WM runs; it uses
	 * streaming stores because rep movsl does not combine on the VRAM
	 * path.  Report the bytes
	 * the presenter really copied: a long flush can be preempted, and a
	 * flush started while another process is inside one is skipped. */
	{
		unsigned int before = vga_present_copied;

		t0 = get_ticks_ms();
		for (i = 0; i < 2; i++) {
			vga_fb_mark_dirty_rect(0, vga_fb_height());
			vga_fb_flush();
		}
		t1 = get_ticks_ms();
		/* one "run" whose size is the bytes actually copied */
		bench("flush screen", 1, vga_present_copied - before, t1 - t0);
	}

	free_pages(layer_px, layer_bytes);
}
