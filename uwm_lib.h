/* Small userland drawing library for the ring-3 WM: bpp-aware fills, copies,
 * 8x8 glyphs, VGA cell grids and RGB565 canvas blits.  No kernel dependencies;
 * the WM maps the shadow buffer and calls these functions directly. */
#ifndef UWM_LIB_H
#define UWM_LIB_H

struct ufont_face;

struct usurf {
	unsigned char *px;
	unsigned int w, h, pitch;
	unsigned int bpp;          /* 16, 24 or 32 */
	struct ufont_face *font;   /* NULL = 8x16 bitmap text */
	int font_px;
};

struct uwm_canvas {
	const unsigned short *px;  /* RGB565, tightly packed */
	unsigned int w, h;
};

/* Pixels written by the compositor primitives; the WM uses it for stats. */
extern unsigned int usurf_pixels;

void usurf_init(struct usurf *s, void *px, unsigned int w, unsigned int h,
		unsigned int pitch, unsigned int bpp);
void usurf_fill(struct usurf *s, int x, int y, unsigned int w,
		unsigned int h, unsigned int rgb);
void usurf_copy(struct usurf *s, int dx, int dy, int sx, int sy,
		unsigned int w, unsigned int h);
void usurf_glyph(struct usurf *s, int x, int y, unsigned char c,
		 unsigned int fg, unsigned int bg, int transparent,
		 unsigned int height,
		 int cx, int cy, unsigned int cw, unsigned int ch);
void usurf_text(struct usurf *s, int x, int y, const char *str,
		unsigned int fg,
		int cx, int cy, unsigned int cw, unsigned int ch);
unsigned int usurf_text_width(struct usurf *s, const char *str);
void usurf_font_attach(struct usurf *s, const char *path, int px);
void usurf_cells(struct usurf *s, const unsigned short *cells,
		 unsigned int cols, unsigned int rows,
		 int x0, int y0,
		 int cx, int cy, unsigned int cw, unsigned int ch);
void usurf_canvas(struct usurf *s, const struct uwm_canvas *c,
		  int x0, int y0,
		  int cx, int cy, unsigned int cw, unsigned int ch);

#endif
