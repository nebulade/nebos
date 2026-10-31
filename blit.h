/* Blit backend: the drawing operations the window manager uses, behind a
 * vtable so a GPU/driver implementation can replace the software one without
 * touching the WM.  Ops paint into the current render target (vga_target());
 * copy() and present() work on the screen.
 *
 * The software table is the only backend today; it is also the fallback for
 * every machine without (or before) a usable accelerator. */
#ifndef BLIT_H
#define BLIT_H

struct surface;
struct gfx_canvas;

struct blit_ops {
	const char *name;

	/* Solid fill with a packed 0xRRGGBB colour. */
	void (*fill)(unsigned int x, unsigned int y, unsigned int w,
		     unsigned int h, unsigned int rgb);

	/* Overlap-safe rectangle copy inside the target. */
	void (*copy)(unsigned int dx, unsigned int dy, unsigned int sx,
		     unsigned int sy, unsigned int w, unsigned int h);

	/* Blend an ARGB8888 source surface onto the target: uniform alpha for
	 * 255, per-pixel source alpha otherwise.  Used by the layer
	 * compositor. */
	void (*blend)(unsigned int x, unsigned int y, unsigned int w,
		      unsigned int h, const struct surface *src,
		      unsigned int sx, unsigned int sy, unsigned int alpha);

	/* Canvas (RGB565) blit clipped to (cx, cy, cw, ch), scaled to the
	 * target's pixel format. */
	void (*canvas)(const struct gfx_canvas *c, unsigned int x,
		       unsigned int y, unsigned int cx, unsigned int cy,
		       unsigned int cw, unsigned int ch);

	/* Make drawn content visible: copy the pending damage rects (up to
	 * max_bytes, 0 = everything), or flip when the backend has two
	 * buffers. */
	void (*present)(unsigned int max_bytes);

	/* Wait for the next frame and swap (no-op for the single-buffered
	 * software path). */
	void (*flip)(void);
};

extern const struct blit_ops *blit;      /* the active backend */
extern const struct blit_ops blit_software;

#endif
