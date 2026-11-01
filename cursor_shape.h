#ifndef CURSOR_SHAPE_INCLUDED
#define CURSOR_SHAPE_INCLUDED

/* Shared 16x16 mouse-pointer image: a white arrow with a one-pixel black
   outline, so the cursor stays visible over white pages and windows too.
   Bit 15 of each row is column 0; the hotspot is the top-left pixel of the
   16x16 box.  The software cursor, the Cirrus two-plane cursor and the
   Radeon ARGB cursor all build their bitmaps from these tables. */

#define CURSOR_SHAPE_WIDTH 16
#define CURSOR_SHAPE_HEIGHT 16

/* White fill (the arrow body). */
static const unsigned short cursor_shape_fill[CURSOR_SHAPE_HEIGHT] = {
	0x0000,
	0x4000,  /* 1....... ........ */
	0x6000,  /* 11...... ........ */
	0x7000,  /* 111..... ........ */
	0x7800,  /* 1111.... ........ */
	0x7C00,  /* 11111... ........ */
	0x7E00,  /* 111111.. ........ */
	0x7F00,  /* 1111111. ........ */
	0x7F80,  /* 11111111 ........ */
	0x7E00,  /* 111111.. ........ */
	0x7000,  /* 111..... ........ */
	0x4000,  /* 1....... ........ */
	0x0000,
	0x0000,
	0x0000,
	0x0000
};

/* Black outline (everything adjacent to the fill). */
static const unsigned short cursor_shape_outline[CURSOR_SHAPE_HEIGHT] = {
	0xC000,  /* 11...... ........  tip */
	0xA000,  /* 1.1..... ........ */
	0x9000,  /* 1..1.... ........ */
	0x8800,  /* 1...1... ........ */
	0x8400,  /* 1....1.. ........ */
	0x8200,  /* 1.....1. ........ */
	0x8100,  /* 1......1 ........ */
	0x8080,  /* 1....... 1....... */
	0x8040,  /* 1....... .1...... */
	0x8180,  /* 1......1 1....... */
	0x8E00,  /* 1...111. ........ */
	0xB000,  /* 1.11.... ........ */
	0x4000   /* .1...... ........ */
};

#endif
