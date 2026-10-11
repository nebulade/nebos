/* USB HID boot devices: mouse + keyboard.
 *
 * A single manager owns the scan/hotplug state for both interrupt-IN
 * endpoints (slot 0 = mouse, slot 1 = keyboard): find, configure, put into
 * boot protocol and keep each endpoint armed.  The mouse reports are handed
 * back to mouse.c; keyboard reports are decoded into PS/2 set-1 scancodes
 * and injected into the regular input queue (io_inject_scancode), so the
 * terminal/WM see them exactly like a PS/2 keyboard. */
#ifndef USBHID_H
#define USBHID_H

/* Bring up the USB subsystem and look for a boot mouse/keyboard.  Safe to
   call when no controller or device exists; the hotplug watcher keeps
   running then. */
void usbhid_init(void);

/* Service both endpoints (called once per WM loop iteration from
   mouse_poll_update()).  Returns 1 when a mouse report is available and
   fills buttons/dx/dy (PS/2 sign convention: dy > 0 = up). */
int usbhid_poll(unsigned int *buttons, int *dx, int *dy);

int usbhid_mouse_attached(void);
int usbhid_keyboard_attached(void);

/* For `mouseinfo`: controller instance, device address, endpoint number,
   endpoint max packet size and the low-speed flag. */
void usbhid_get_info(unsigned int *ctrl, unsigned int *addr,
		     unsigned int *ep, unsigned int *mps, int *low);

#endif
