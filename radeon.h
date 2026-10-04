/* ATI Radeon (R200 / RV280) hardware cursor. */
#ifndef RADEON_H
#define RADEON_H

/* Called by ati_init() when the active adapter is a Radeon. */
void radeon_init(unsigned int bus, unsigned int dev, unsigned int device_id);
int  radeon_available(void);

/* Hardware cursor (64x64, 2 bpp). */
void radeon_cursor_setup(void);
void radeon_cursor_move(unsigned int x, unsigned int y);
void radeon_cursor_show(int show);

#endif
