/* UHCI (USB 1.1) host controller driver.
   PCI discovery, register access, frame-list / queue-head / transfer-
   descriptor schedule, and root-hub port control. Polling based (no IRQ). */
#ifndef UHCI_H
#define UHCI_H

int uhci_init(void);   /* find + reset/configure the controller; 0 on success */

void uhci_pci_info(unsigned int *bus, unsigned int *dev, unsigned int *func,
		   unsigned int *io_base);

unsigned int uhci_root_port_count(void);
int uhci_root_port_connected(unsigned int port);
int uhci_root_port_low_speed(unsigned int port);
int uhci_root_port_reset(unsigned int port);

/* Control transfer on endpoint 0. Returns bytes transferred (>= 0) or -1.
   mps = device max packet size for endpoint 0 (used to split the data stage).
   dir_in: 1 = data stage IN (device->host), 0 = OUT or no data stage. */
int uhci_control_transfer(unsigned int dev_addr, unsigned int mps, int low_speed,
			  const unsigned char *setup, unsigned char *data,
			  unsigned int len, int dir_in);

#endif
