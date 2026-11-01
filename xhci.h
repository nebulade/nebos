/* xHCI (USB 3.0) host controller driver: implements the usb_hcd_ops vtable
   declared in usb_hcd.h.  USB2 root ports only for now (the USB3 ports are
   left alone so the existing HID stack can be validated first). */
#ifndef XHCI_H
#define XHCI_H

#include "usb_hcd.h"

extern const struct usb_hcd_ops usb_xhci_ops;

#endif
