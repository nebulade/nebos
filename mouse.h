#ifndef MOUSE_H
#define MOUSE_H

void mouse_init(void);
void mouse_poll_update(void);
/* IRQ12 handler: PS/2 AUX data, used when a hardware cursor is active. */
void mouse_irq(void);

int mouse_is_present(void);
/* 1 when a PS/2 mouse was detected; the protocol accessors below are
   PS/2-specific (the USB backend lives in usbhid.h). */
int mouse_ps2_present(void);
int mouse_streaming_enabled(void);
unsigned int mouse_device_id(void);
int mouse_cursor_visible(void);
/* Active cursor backend: 0 = software, 1 = Cirrus, 2 = Radeon, 3 = Intel. */
int mouse_cursor_backend(void);
/* Human-readable form of the above ("software", "nvidia hw", ...). */
const char *mouse_cursor_backend_name(void);
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

/* While a WM frame owns the shadow buffer, movement must update the position
 * and buttons without drawing a software cursor underneath the frame.  The
 * WM erases/redraws the cursor itself at BEGIN/PRESENT. */
void mouse_set_auto_draw(int on);

#endif
