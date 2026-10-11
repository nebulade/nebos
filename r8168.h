/* Realtek RTL8111/8168 (RTL8112L) PCIe Gigabit Ethernet driver. */
#ifndef R8168_H
#define R8168_H

/* Probe + reset + configure the NIC.  Idempotent; 0 on success, -1 if absent. */
int r8168_init(void);

void r8168_get_mac(unsigned char out[6]);
int  r8168_rx(unsigned char *buf, unsigned int maxlen);
int  r8168_tx(const unsigned char *buf, unsigned int len);

/* Enable/disable PHY loopback for the self-test (see r8168.c). */
int  r8168_set_loopback(int on);

void r8168_print_info(void);
void r8168_dump_regs(void);
void r8168_reset(void);

/* netdev adapter (see netdev.h). */
#include "netdev.h"
extern const struct netdev r8168_netdev;

#endif
