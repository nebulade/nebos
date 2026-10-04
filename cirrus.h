/* Cirrus Logic GD5446 hardware acceleration (BitBLT engine). */
#ifndef CIRRUS_H
#define CIRRUS_H

void cirrus_init(void);
int  cirrus_available(void);

/* Copy a pixel rectangle from (sx,sy) to (dx,dy), both within VRAM. */
void cirrus_blit(unsigned int sx, unsigned int sy,
		 unsigned int dx, unsigned int dy,
		 unsigned int w, unsigned int h);

/* Fill a pixel rectangle in VRAM with a solid color. */
void cirrus_fill(unsigned int x, unsigned int y,
		 unsigned int w, unsigned int h,
		 unsigned int r, unsigned int g, unsigned int b);

/* Hardware cursor (32x32). */
void cirrus_cursor_setup(void);
void cirrus_cursor_move(unsigned int x, unsigned int y);
void cirrus_cursor_show(int show);

#endif
