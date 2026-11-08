/* 8x16 text-cell renderer for window content canvases.
 *
 * The terminal keeps its 64x32 char+attribute grid; the window server renders
 * damaged cells into an RGB565 canvas and the WM composites that canvas like
 * any other window content.  Freestanding and host-testable. */
#ifndef TEXT_CANVAS_H
#define TEXT_CANVAS_H

#define TEXT_CELL_W 8
#define TEXT_CELL_H 16

struct text_canvas {
	unsigned short *px;   /* RGB565, w*h, tightly packed */
	unsigned int w, h;
};

/* Render the cell block [col0, col0+ncols) x [row0, row0+nrows) of a
 * cols x rows cell grid at the canvas top-left.  Every pixel of the block is
 * written (background included).  `cursor` is a linear cell index; the cursor
 * bar is drawn when visible and focused.  Out-of-range cells are clipped. */
void text_canvas_render(struct text_canvas *c,
			const unsigned short *cells,
			unsigned int cols, unsigned int rows,
			unsigned int col0, unsigned int row0,
			unsigned int ncols, unsigned int nrows,
			unsigned int cursor, int cursor_visible, int focused);

#endif
