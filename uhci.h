/* UHCI (USB 1.1) host controller driver: implements the usb_hcd_ops vtable
   declared in usb_hcd.h. */
#ifndef UHCI_H
#define UHCI_H

#include "usb_hcd.h"

extern const struct usb_hcd_ops usb_uhci_ops;

#endif
