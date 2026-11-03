/* EHCI (USB 2.0) host controller driver: implements the usb_hcd_ops vtable
   declared in usb_hcd.h.  Only high-speed devices are handled directly;
   low/full-speed devices stay with the companion UHCI/OHCI controller. */
#ifndef EHCI_H
#define EHCI_H

#include "usb_hcd.h"

extern const struct usb_hcd_ops usb_ehci_ops;

#endif
