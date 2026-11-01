/* ASIX AX88772 USB 2.0 Ethernet driver. */
#ifndef ASIX_H
#define ASIX_H

#include "netdev.h"

extern const struct netdev asix_netdev;

/* Register the USB device probe (call before the first USB scan). */
void asix_register(void);

#endif
