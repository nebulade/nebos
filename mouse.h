#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_poll_update(void);

int mouse_is_present(void);
/* 1 when a PS/2 mouse was detected; the protocol accessors below are
   PS/2-specific (the USB backend lives in usbhid.h). */
int mouse_ps2_present(void);
int mouse_streaming_enabled(void);
unsigned int mouse_device_id(void);
int mouse_cursor_visible(void);
unsigned int mouse_packet_size(void);

/* Current cursor position (hotspot) in framebuffer pixels. */
unsigned int mouse_get_x(void);
unsigned int mouse_get_y(void);
/* Button state bitmask: bit0 = left, bit1 = right, bit2 = middle. */
unsigned int mouse_buttons_state(void);
/* Restore the saved background under the cursor (erase the cursor). Call
   before a scene redraw so mouse_redraw_cursor() re-saves a clean background. */
void mouse_cursor_erase(void);
/* Re-save the region under the cursor and redraw it on top (after the WM
   redraws windows underneath a stationary cursor). */
void mouse_redraw_cursor(void);

#endif
