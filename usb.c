/* USB core: device enumeration, descriptor parsing, and hub recursion.
   Drives the UHCI controller to build the device tree for `lsusb`. */

#include "io.h"
#include "timer.h"
#include "uhci.h"
#include "usb.h"

/* Standard request types. */
#define RT_DEV2HOST_STD 0x80u
#define RT_HOST2DEV_STD 0x00u
#define RT_DEV2HOST_CLS 0xA0u
#define RT_DEV2HOST_OTH 0xA3u
#define RT_HOST2DEV_OTH 0x23u

/* Standard requests. */
#define REQ_GET_STATUS     0x00u
#define REQ_CLEAR_FEATURE  0x01u
#define REQ_SET_FEATURE    0x03u
#define REQ_SET_ADDRESS    0x05u
#define REQ_GET_DESCRIPTOR 0x06u

/* Descriptor types. */
#define DESC_DEVICE 0x01u
#define DESC_HUB    0x29u

/* Hub feature selectors. */
#define FEAT_PORT_RESET        4u
#define FEAT_PORT_POWER        8u
#define FEAT_C_PORT_CONNECTION 16u

/* Hub port status bits. */
#define PSTAT_CONNECT   0x0001u
#define PSTAT_LOW_SPEED 0x0200u

static unsigned int devnum;

static unsigned char dev_desc[18];
static unsigned char desc8[8];
static unsigned char hub_desc[16];
static unsigned char port_st[4];

static void put_uint(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;
	*p = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	while (*p)
		io_putchar(*p++);
}

static void put_hex(unsigned int n, unsigned int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned int i;
	for (i = 0; i < digits; i++)
		io_putchar(hex[(n >> ((digits - 1 - i) * 4)) & 0xF]);
}

static void msleep(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static int ctrl_in(unsigned int addr, unsigned int mps, int low,
		   unsigned char rt, unsigned char req, unsigned short value,
		   unsigned short index, unsigned char *buf, unsigned int len)
{
	unsigned char setup[8];
	setup[0] = rt;
	setup[1] = req;
	setup[2] = (unsigned char)(value & 0xFF);
	setup[3] = (unsigned char)((value >> 8) & 0xFF);
	setup[4] = (unsigned char)(index & 0xFF);
	setup[5] = (unsigned char)((index >> 8) & 0xFF);
	setup[6] = (unsigned char)(len & 0xFF);
	setup[7] = (unsigned char)((len >> 8) & 0xFF);
	return uhci_control_transfer(addr, mps, low, setup, buf, len, 1);
}

static int ctrl_out(unsigned int addr, unsigned int mps, int low,
		    unsigned char rt, unsigned char req, unsigned short value,
		    unsigned short index)
{
	unsigned char setup[8];
	setup[0] = rt;
	setup[1] = req;
	setup[2] = (unsigned char)(value & 0xFF);
	setup[3] = (unsigned char)((value >> 8) & 0xFF);
	setup[4] = (unsigned char)(index & 0xFF);
	setup[5] = (unsigned char)((index >> 8) & 0xFF);
	setup[6] = 0;
	setup[7] = 0;
	return uhci_control_transfer(addr, mps, low, setup, 0, 0, 0);
}

static const char *class_name(unsigned int cls)
{
	switch (cls) {
	case 0x00: return "Per-interface";
	case 0x01: return "Audio";
	case 0x02: return "Communications";
	case 0x03: return "Human Interface";
	case 0x05: return "Physical";
	case 0x06: return "Image";
	case 0x07: return "Printer";
	case 0x08: return "Mass Storage";
	case 0x09: return "Hub";
	case 0x0A: return "CDC Data";
	case 0x0B: return "Smart Card";
	case 0x0D: return "Content Security";
	case 0x0E: return "Video";
	case 0x0F: return "Personal Healthcare";
	case 0xE0: return "Wireless";
	case 0xEF: return "Miscellaneous";
	case 0xFE: return "Application";
	case 0xFF: return "Vendor-specific";
	default: return "Unknown";
	}
}

static void indent(unsigned int depth)
{
	unsigned int i;
	for (i = 0; i < depth; i++)
		io_putstr("  ");
}

/* wPortStatus (little-endian) from the GET_PORT_STATUS buffer. */
static unsigned int port_status_word(void)
{
	return (unsigned int)port_st[0] | ((unsigned int)port_st[1] << 8);
}

static void enumerate_hub(unsigned int addr, unsigned int mps, int low,
			  unsigned int depth);
static void enumerate_device(unsigned int addr, unsigned int mps, int low,
			     unsigned int depth);
static void enumerate_new_device(int low, unsigned int depth);

static void enumerate_device(unsigned int addr, unsigned int mps, int low,
			     unsigned int depth)
{
	unsigned int vendor, product, cls, subcls, proto;

	if (ctrl_in(addr, mps, low, RT_DEV2HOST_STD, REQ_GET_DESCRIPTOR,
		    (unsigned short)(DESC_DEVICE << 8), 0, dev_desc, 18) < 0) {
		indent(depth);
		io_putstr("(device descriptor read failed)\n");
		return;
	}

	vendor  = dev_desc[8]  | ((unsigned int)dev_desc[9]  << 8);
	product = dev_desc[10] | ((unsigned int)dev_desc[11] << 8);
	cls     = dev_desc[4];
	subcls  = dev_desc[5];
	proto   = dev_desc[6];

	indent(depth);
	io_putstr("Bus 001 Device ");
	put_uint(addr);
	io_putstr(": ID ");
	put_hex(vendor, 4);
	io_putchar(':');
	put_hex(product, 4);
	io_putchar(' ');
	io_putstr(class_name(cls));
	io_putstr(" [");
	put_hex(cls, 2);
	io_putchar('/');
	put_hex(subcls, 2);
	io_putchar('/');
	put_hex(proto, 2);
	io_putstr("]\n");

	if (cls == 0x09)
		enumerate_hub(addr, mps, low, depth + 1);
}

static void enumerate_hub(unsigned int addr, unsigned int mps, int low,
			  unsigned int depth)
{
	unsigned int nports, p;

	if (ctrl_in(addr, mps, low, RT_DEV2HOST_CLS, REQ_GET_DESCRIPTOR,
		    (unsigned short)(DESC_HUB << 8), 0, hub_desc,
		    sizeof(hub_desc)) < 0)
		return;

	nports = hub_desc[2];
	if (nports > 8)
		nports = 8;

	for (p = 1; p <= nports; p++) {
		int low2;

		if (ctrl_in(addr, mps, low, RT_DEV2HOST_OTH, REQ_GET_STATUS,
			    0, (unsigned short)p, port_st, 4) < 0)
			continue;
		if (!(port_status_word() & PSTAT_CONNECT))
			continue;

		/* Power and reset the downstream port. */
		ctrl_out(addr, mps, low, RT_HOST2DEV_OTH, REQ_SET_FEATURE,
			 FEAT_PORT_POWER, (unsigned short)p);
		if (ctrl_out(addr, mps, low, RT_HOST2DEV_OTH, REQ_SET_FEATURE,
			     FEAT_PORT_RESET, (unsigned short)p) < 0)
			continue;
		msleep(30);

		ctrl_out(addr, mps, low, RT_HOST2DEV_OTH, REQ_CLEAR_FEATURE,
			 FEAT_C_PORT_CONNECTION, (unsigned short)p);

		/* Re-read status to learn the child's speed. */
		low2 = 0;
		if (ctrl_in(addr, mps, low, RT_DEV2HOST_OTH, REQ_GET_STATUS,
			    0, (unsigned short)p, port_st, 4) >= 0) {
			if (port_status_word() & PSTAT_LOW_SPEED)
				low2 = 1;
		}

		enumerate_new_device(low2, depth);
	}
}

static void enumerate_new_device(int low, unsigned int depth)
{
	unsigned int mps;

	devnum++;
	if (devnum > 127) {
		indent(depth);
		io_putstr("(too many devices)\n");
		return;
	}

	/* First 8 bytes at address 0 (learn max packet size). */
	if (ctrl_in(0, 8, low, RT_DEV2HOST_STD, REQ_GET_DESCRIPTOR,
		    (unsigned short)(DESC_DEVICE << 8), 0, desc8, 8) < 0) {
		indent(depth);
		io_putstr("(no response at address 0)\n");
		return;
	}

	mps = desc8[7];
	if (mps == 0)
		mps = 8;

	/* Assign an address. */
	if (ctrl_out(0, 8, low, RT_HOST2DEV_STD, REQ_SET_ADDRESS,
		     (unsigned short)devnum, 0) < 0) {
		indent(depth);
		io_putstr("(SET_ADDRESS failed)\n");
		return;
	}
	msleep(5);

	enumerate_device(devnum, mps, low, depth);
}

void usb_enumerate_and_print(void)
{
	unsigned int bus, dev, func, base;
	unsigned int p;

	devnum = 0;

	if (uhci_init() != 0) {
		io_putstr("lsusb: no UHCI host controller found\n");
		return;
	}

	uhci_pci_info(&bus, &dev, &func, &base);
	io_putstr("UHCI Host Controller (PCI ");
	put_hex(bus, 2);
	io_putchar(':');
	put_hex(dev, 2);
	io_putchar('.');
	put_hex(func, 1);
	io_putstr(", I/O 0x");
	put_hex(base, 4);
	io_putstr(")\n");

	for (p = 0; p < uhci_root_port_count(); p++) {
		if (!uhci_root_port_connected(p))
			continue;
		if (uhci_root_port_reset(p) != 0)
			continue;
		enumerate_new_device(uhci_root_port_low_speed(p), 1);
	}
}
