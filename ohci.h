/* OHCI (USB 1.1) host controller driver: implements the usb_hcd_ops vtable
   declared in usb_hcd.h. */
#ifndef OHCI_H
#define OHCI_H

#include "usb_hcd.h"

extern const struct usb_hcd_ops usb_ohci_ops;

#endif
