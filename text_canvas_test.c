/* Host test for the 8x16 terminal-cell renderer (make test-textcanvas).
 * Exercises the renderer directly; no kernel headers are involved. */

#include <stdio.h>
#include "text_canvas.h"
#include "font_8x16.h"

#define COLS 64u
#define ROWS 32u
#define W (COLS * TEXT_CELL_W)
#define H (ROWS * TEXT_CELL_H)

/* Hash of the fully rendered reference grid.  Regenerate only when the font,
 * palette or cell metrics intentionally change. */
#define GOLDEN 0x8598C6C9u

static unsigned short cells[COLS * ROWS];
static unsigned short fb[W * H];
static int failures;

/* Same standard VGA palette as text_canvas.c, in RGB565. */
static const unsigned short pal565[16] = {
	0x0000, 0x0015, 0x0540, 0x0555,
	0xA800, 0xA815, 0xAAA0, 0xAD55,
	0x52AA, 0x52BF, 0x57EA, 0x57FF,
	0xFAAA, 0xFABF, 0xFFEA, 0xFFFF,
};

static void expect(unsigned int x, unsigned int y, unsigned short want,
		   const char *what)
{
	unsigned short got;

	if (x >= W || y >= H) {
		printf("FAIL %s: (%u,%u) out of range\n", what, x, y);
		failures++;
		return;
	}
	got = fb[y * W + x];
	if (got != want) {
		printf("FAIL %s at (%u,%u): got 0x%04X want 0x%04X\n",
		       what, x, y, got, want);
		failures++;
	}
}

static int glyph_bit(char ch, unsigned int row, unsigned int col)
{
	return (font_8x16[(unsigned int)(unsigned char)ch * FONT_8X16_ROWS +
			  row] >> (7 - col)) & 1;
}

static void fill_cells(unsigned short value)
{
	unsigned int i;

	for (i = 0; i < COLS * ROWS; i++)
		cells[i] = value;
}

static void put_text(unsigned int col, unsigned int row, const char *s,
		     unsigned char attr)
{
	unsigned int i;

	for (i = 0; s[i] && col + i < COLS; i++)
		cells[row * COLS + col + i] =
			(unsigned short)(((unsigned short)attr << 8) |
					(unsigned char)s[i]);
}

static unsigned int hash565(const unsigned short *px, unsigned int n)
{
	unsigned int h = 2166136261u;
	unsigned int i;

	for (i = 0; i < n; i++) {
		h = (h ^ (px[i] & 0xFF)) * 16777619u;
		h = (h ^ (px[i] >> 8)) * 16777619u;
	}
	return h;
}

int main(void)
{
	struct text_canvas c = { fb, W, H };
	unsigned int cursor = 3 * COLS + 5;
	unsigned int i, hash;

	fill_cells(0x0720);              /* space, light gray on black */
	put_text(0, 0, "Nebs OS 8x16", 0x0F);
	put_text(0, 1, "0123456789", 0x1E);
	put_text(2, 2, "ABCDEFGH", 0x4A);
	cells[1 * COLS + 12] = 0x1E20;   /* blue background space */
	cells[3 * COLS + 4] = 0x4A20;    /* red background space sentinel */
	cells[3 * COLS + 5] = 0x0020;    /* black space under the cursor */

	/* The full render must cover every pixel. */
	for (i = 0; i < W * H; i++)
		fb[i] = 0xDEAD;
	text_canvas_render(&c, cells, COLS, ROWS, 0, 0, COLS, ROWS,
			   cursor, 1, 1);
	for (i = 0; i < W * H; i++) {
		if (fb[i] == 0xDEAD) {
			printf("FAIL full render left sentinel at pixel %u\n", i);
			failures++;
			break;
		}
	}

	hash = hash565(fb, W * H);
	if (hash != GOLDEN) {
		printf("FAIL full render hash 0x%08X, expected 0x%08X\n",
		       hash, GOLDEN);
		failures++;
	}

	/* 8x16 glyph mapping: 'A' at (2,2), attr 0x4A = light green on red. */
	{
		unsigned int row, bit;
		int set_seen = 0, clear_seen = 0;

		for (row = 0; row < FONT_8X16_ROWS; row++) {
			for (bit = 0; bit < 8; bit++) {
				unsigned int x = 2 * TEXT_CELL_W + bit;
				unsigned int y = 2 * TEXT_CELL_H + row;
				int on = glyph_bit('A', row, bit);

				if (on && !set_seen) {
					expect(x, y, pal565[10], "glyph set");
					set_seen = 1;
				}
				if (!on && !clear_seen) {
					expect(x, y, pal565[4], "glyph clear");
					clear_seen = 1;
				}
			}
		}
		if (!set_seen || !clear_seen) {
			printf("FAIL glyph test could not find set/clear bits\n");
			failures++;
		}
	}

	/* Cell boundary: row 0 background at y=15, row 1 at y=16. */
	expect(12 * TEXT_CELL_W, 15, pal565[0], "row 0 bottom");
	expect(12 * TEXT_CELL_W, 16, pal565[1], "row 1 top");

	/* Cursor: black space at (5,3), white 2x16 bar. */
	expect(5 * TEXT_CELL_W + 0, 3 * TEXT_CELL_H + 0, pal565[15],
	       "cursor x0");
	expect(5 * TEXT_CELL_W + 1, 3 * TEXT_CELL_H + 0, pal565[15],
	       "cursor x1");
	expect(5 * TEXT_CELL_W + 2, 3 * TEXT_CELL_H + 0, pal565[0],
	       "cursor x2");
	expect(5 * TEXT_CELL_W + 0, 3 * TEXT_CELL_H + 15, pal565[15],
	       "cursor bottom");
	expect(5 * TEXT_CELL_W + 0, 4 * TEXT_CELL_H, pal565[0],
	       "cursor next row");

	/* Unfocused windows must not show the cursor. */
	text_canvas_render(&c, cells, COLS, ROWS, 0, 0, COLS, ROWS,
			   cursor, 1, 0);
	expect(5 * TEXT_CELL_W + 0, 3 * TEXT_CELL_H + 0, pal565[0],
	       "cursor unfocused");

	/* Block update: only cell (5,3) changes. */
	expect(4 * TEXT_CELL_W, 3 * TEXT_CELL_H, pal565[4],
	       "sentinel before block");
	cells[3 * COLS + 5] = (0x0F << 8) | 'X';
	text_canvas_render(&c, cells, COLS, ROWS, 5, 3, 1, 1,
			   0xFFFFFFFFu, 0, 0);
	expect(4 * TEXT_CELL_W, 3 * TEXT_CELL_H, pal565[4],
	       "sentinel after block");
	expect(5 * TEXT_CELL_W, 4 * TEXT_CELL_H, pal565[0],
	       "row 4 after block");
	{
		unsigned int row, bit;
		int set_seen = 0, clear_seen = 0;

		for (row = 0; row < FONT_8X16_ROWS; row++) {
			for (bit = 0; bit < 8; bit++) {
				unsigned int x = 5 * TEXT_CELL_W + bit;
				unsigned int y = 3 * TEXT_CELL_H + row;
				int on = glyph_bit('X', row, bit);

				if (on && !set_seen) {
					expect(x, y, pal565[15], "X set");
					set_seen = 1;
				}
				if (!on && !clear_seen) {
					expect(x, y, pal565[0], "X clear");
					clear_seen = 1;
				}
			}
		}
		if (!set_seen || !clear_seen) {
			printf("FAIL X glyph test could not find bits\n");
			failures++;
		}
	}

	/* A block at the last cell must clamp without touching the canvas
	 * bounds, and still render the cell. */
	cells[(ROWS - 1) * COLS + (COLS - 1)] = (0x0F << 8) | 'X';
	text_canvas_render(&c, cells, COLS, ROWS, COLS - 1, ROWS - 1, 4, 4,
			   0xFFFFFFFFu, 0, 0);
	{
		unsigned int row, bit;
		int set_seen = 0;

		for (row = 0; row < FONT_8X16_ROWS && !set_seen; row++)
			for (bit = 0; bit < 8; bit++)
				if (glyph_bit('X', row, bit)) {
					expect((COLS - 1) * TEXT_CELL_W + bit,
					       (ROWS - 1) * TEXT_CELL_H + row,
					       pal565[15], "edge block");
					set_seen = 1;
					break;
				}
		if (!set_seen) {
			printf("FAIL edge block glyph bits not found\n");
			failures++;
		}
	}

	if (failures) {
		printf("%d text canvas test(s) failed\n", failures);
		return 1;
	}
	printf("all text canvas tests passed\n");
	return 0;
}
