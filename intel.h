/* Intel Ironlake (Gen5) hardware cursor. */
#ifndef INTEL_H
#define INTEL_H

/* Probe the Intel display engine and prepare a cursor image in the GTT.
 * Called from gpu_init() for the active VGA device. */
void intel_init(unsigned int bus, unsigned int dev, unsigned int device_id);

/* 1 when the hardware cursor is fully set up and usable. */
int intel_available(void);

/* Enable the cursor at (x,y); returns 0 on success and -1 when the cursor
 * image/GTT was not prepared (the caller then keeps the software cursor). */
int intel_cursor_setup(unsigned int x, unsigned int y);

/* Move the hardware cursor. */
void intel_cursor_move(unsigned int x, unsigned int y);

#endif
