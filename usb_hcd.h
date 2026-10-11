/* USB host controller driver (HCD) interface.
 *
 * usb.c drives enumeration, bus scans and the interrupt-IN consumers (USB
 * mouse + keyboard) through this vtable; uhci.c and ohci.c each implement it
 * for their controller type.  One "controller instance" is one PCI function
 * (MCP79 exposes four OHCI ones, for example). */
#ifndef USB_HCD_H
#define USB_HCD_H

#define USB_HCD_MAX_CTRL 4

/* Interrupt-IN endpoints serviced concurrently; usbhid manages which
   slot carries which device (mice + keyboards, composite devices). */
#define USB_HCD_INT_SLOTS 6

/* One interface of one device, filled in during a scan and passed to the
 * scan probe.  ep_addr is 0 when the interface has no interrupt-IN endpoint. */
struct usb_iface_info {
	unsigned int ctrl;        /* controller instance index */
	unsigned int addr;        /* device address on that controller's bus */
	unsigned int mps;         /* device EP0 max packet size */
	int low_speed;
	unsigned int port;        /* root port the device hangs off (0xFF: behind a hub) */
	unsigned int vendor, product;
	unsigned int cls, subcls, proto;   /* interface class/subclass/protocol */
	unsigned int iface_num;
	unsigned int config_value;
	unsigned int ep_addr;     /* endpoint address byte (0x81 = EP1 IN) */
	unsigned int ep_mps;
	unsigned int ep_interval;
};

/* Called once per interface during a scan; return 1 when claimed. */
typedef int (*usb_probe_fn)(const struct usb_iface_info *info);

struct usb_hcd_ops {
	const char *name;

	/* Find + reset the controller(s).  Returns the number of instances
	 * found (0 = none).  Idempotent: only brings hardware up on the
	 * first successful call. */
	int (*init)(void);
	unsigned int (*count)(void);

	/* PCI location and register base of one instance (for the `lsusb`
	 * header).  *mmio is 1 for a memory-mapped register block. */
	void (*info)(unsigned int ctrl, unsigned int *bus, unsigned int *dev,
		     unsigned int *func, unsigned int *base, int *mmio);

	/* Control transfer on endpoint 0.  Returns bytes transferred
	 * (>= 0) or -1.  mps = EP0 max packet size (used to split the data
	 * stage).  dir_in: 1 = IN data stage, 0 = OUT or no data stage. */
	int (*control_transfer)(unsigned int ctrl, unsigned int addr,
				unsigned int mps, int low_speed,
				const unsigned char *setup, unsigned char *data,
				unsigned int len, int dir_in);

	/* Root hub ports of one instance.  Port numbers are 0-based. */
	unsigned int (*port_count)(unsigned int ctrl);
	int (*port_connected)(unsigned int ctrl, unsigned int port);
	int (*port_low_speed)(unsigned int ctrl, unsigned int port);
	int (*port_reset)(unsigned int ctrl, unsigned int port);
	/* Returns 1 (and clears the hardware latch) when a connection change
	 * was detected since the last call. */
	int (*port_take_change)(unsigned int ctrl, unsigned int port);

	/* Interrupt-IN endpoints (one slot per HID device; see
	 * USB_HCD_INT_SLOTS).  The transfer stays armed in the schedule;
	 * the controller retries NAKs itself.  poll: > 0 report bytes
	 * (auto re-armed), 0 nothing yet, -1 error (still armed, toggle
	 * preserved; the caller may count errors and re-init). */
	int (*int_in_start)(unsigned int ctrl, unsigned int slot,
			    unsigned int addr, unsigned int ep,
			    unsigned int mps, int low_speed);
	int (*int_in_poll)(unsigned int slot, unsigned char *buf,
			   unsigned int len);
	void (*int_in_stop)(unsigned int slot);
};

extern const struct usb_hcd_ops usb_uhci_ops;
extern const struct usb_hcd_ops usb_ohci_ops;

#endif
