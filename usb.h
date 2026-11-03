/* USB core: device enumeration, descriptor parsing, hub recursion, bus
   scans and the hotplug watcher.  Controller-specific work (transfers, root
   ports) goes through the usb_hcd_ops vtable (uhci.c / ohci.c / ehci.c /
   xhci.c). */
#ifndef USB_H
#define USB_H

#include "usb_hcd.h"

/* `lsusb` command: full scan, print the tree. */
void usb_enumerate_and_print(void);

/* Find + bring up every host controller; 0 when at least one is available. */
int usb_hcd_init(void);
const char *usb_hcd_name(void);
/* Name of the controller type behind one global controller index. */
const char *usb_hcd_name_ctrl(unsigned int ctrl);

/* Full bus scan: resets every connected root port, re-enumerates and calls
   the probe for each interface (probe may be NULL to print instead).
   Returns 0, -1 (no controller) or -2 (a scan is already running); every
   completed scan bumps the generation counter. */
int usb_scan(usb_probe_fn probe);

/* Cheap periodic watcher: enumerates devices on root ports whose connect
   state changed since the last call (internally throttled).  Returns the
   number of ports with a new device. */
int usb_hotplug_poll(usb_probe_fn probe);

/* Incremented by every scan; consumers re-init when it changes. */
unsigned int usb_generation(void);

/* Root-port query (used to watch the port the mouse hangs off). */
int usb_port_connected(unsigned int ctrl, unsigned int port);

/* Control request helpers.  rt is the raw bmRequestType (standard 0x80/0x00,
   class 0xA0/0x20, other 0xA3/0x23); the rest is the raw setup fields. */
int usb_control_in(unsigned int ctrl, unsigned int addr, unsigned int mps,
		   int low_speed, unsigned char rt, unsigned char req,
		   unsigned short value, unsigned short index,
		   unsigned char *buf, unsigned int len);
int usb_control_out(unsigned int ctrl, unsigned int addr, unsigned int mps,
		    int low_speed, unsigned char rt, unsigned char req,
		    unsigned short value, unsigned short index);
int usb_control_out_data(unsigned int ctrl, unsigned int addr,
			 unsigned int mps, int low_speed, unsigned char rt,
			 unsigned char req, unsigned short value,
			 unsigned short index, const unsigned char *buf,
			 unsigned int len);

/* Bulk transfers: bulk_in is start-or-poll (see usb_hcd.h). */
int usb_bulk_out(unsigned int ctrl, unsigned int addr, unsigned int ep,
		 unsigned int mps, const unsigned char *buf,
		 unsigned int len);
int usb_bulk_in(unsigned int ctrl, unsigned int addr, unsigned int ep,
		unsigned int mps, unsigned char *buf, unsigned int len);

/* Device-level probe for drivers that do not use HID interfaces (the
 * USB Ethernet driver parses the whole configuration itself).  Return 1
 * when the device was claimed.  Called for every non-hub device during a
 * scan, before the per-interface HID probe. */
typedef int (*usb_device_probe_fn)(unsigned int ctrl, unsigned int addr,
				   unsigned int mps, int low_speed,
				   unsigned int port, unsigned int vendor,
				   unsigned int product);
void usb_register_device_probe(usb_device_probe_fn fn);

/* Interrupt-IN endpoints (slots 0..USB_HCD_INT_SLOTS-1; usbhid assigns
   one per HID interface it claims). */
int usb_int_in_start(unsigned int ctrl, unsigned int slot, unsigned int addr,
		     unsigned int ep, unsigned int mps, int low_speed);
int usb_int_in_poll(unsigned int slot, unsigned char *buf, unsigned int len);
void usb_int_in_stop(unsigned int slot);

#endif
