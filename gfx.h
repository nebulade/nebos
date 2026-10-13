/* Window-local RGB565 canvas + blit into the VGA shadow buffer. */
#ifndef GFX_H
#define GFX_H

struct gfx_canvas {
	unsigned short *px;   /* w*h RGB565, page-aligned; NULL = none */
	unsigned int w, h;
	unsigned int seq;     /* frames the owner has presented */
	unsigned int drawn;   /* last seq the WM composited (catch-up safe) */
};

/* Allocate a canvas of w*h pixels (zeroed; fresh memory is not).  Returns 0/-1. */
int gfx_alloc(struct gfx_canvas *c, unsigned int w, unsigned int h);
void gfx_free(struct gfx_canvas *c);

/* Copy the canvas into the shadow buffer at pixel origin (x0, y0), converting
 * RGB565 to the mode's 16/24/32 bpp, and mark the affected rows dirty. */
void gfx_blit(const struct gfx_canvas *c, unsigned int x0, unsigned int y0);

/* Same, but only the part whose destination bounds intersect the clip rect
 * (cx, cy, cw, ch) - used to restore window slivers. */
void gfx_blit_clip(const struct gfx_canvas *c, unsigned int x0, unsigned int y0,
		   unsigned int cx, unsigned int cy, unsigned int cw, unsigned int ch);

#endif
