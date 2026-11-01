/* USB core: device enumeration, descriptor parsing, hub recursion, bus scans
   and the hotplug watcher.  Controller-specific work goes through the
   usb_hcd_ops vtable (uhci.c / ohci.c). */

#include "io.h"
#include "timer.h"
#include "usb.h"
#include "usb_hcd.h"

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
#define DESC_DEVICE    0x01u
#define DESC_CONFIG    0x02u
#define DESC_INTERFACE 0x04u
#define DESC_ENDPOINT  0x05u
#define DESC_HUB       0x29u

/* Hub feature selectors. */
#define FEAT_PORT_RESET        4u
#define FEAT_PORT_POWER        8u
#define FEAT_C_PORT_CONNECTION 16u

/* Hub port status bits. */
#define PSTAT_CONNECT   0x0001u
#define PSTAT_LOW_SPEED 0x0200u

/* How much of a configuration descriptor we read/parse. */
#define USB_CONFIG_MAX 128

/* Hotplug watcher period. */
#define HOTPLUG_MS 250

/* Bring-up diagnostics to COM1 (also mirrored by the network console on
   boards without serial).  Set to 0 to silence. */
#define USB_DEBUG 0

#if USB_DEBUG
#include "serial.h"
static void dbg(const char *s)
{
	serial_puts(SERIAL_COM1, s);
}
static void dbg_dec(const char *s, unsigned int n)
{
	serial_puts(SERIAL_COM1, s);
	serial_putdec(SERIAL_COM1, n);
	serial_putc(SERIAL_COM1, '\n');
}
static void dbg_port(unsigned int ctrl, unsigned int port, unsigned int conn,
		     unsigned int low)
{
	serial_puts(SERIAL_COM1, "usb: ctrl ");
	serial_putdec(SERIAL_COM1, ctrl);
	serial_puts(SERIAL_COM1, " port ");
	serial_putdec(SERIAL_COM1, port);
	serial_puts(SERIAL_COM1, " connected ");
	serial_putdec(SERIAL_COM1, conn);
	serial_puts(SERIAL_COM1, " ls ");
	serial_putdec(SERIAL_COM1, low);
	serial_putc(SERIAL_COM1, '\n');
}
#else
static void dbg(const char *s) { (void)s; }
static void dbg_dec(const char *s, unsigned int n) { (void)s; (void)n; }
static void dbg_port(unsigned int ctrl, unsigned int port, unsigned int conn,
		     unsigned int low)
{
	(void)ctrl;
	(void)port;
	(void)conn;
	(void)low;
}
#endif

static const struct usb_hcd_ops *hcd;
static unsigned int generation;
static unsigned int next_addr[USB_HCD_MAX_CTRL];
static usb_probe_fn current_probe;
static usb_device_probe_fn device_probe;
static int print_devices;
static int scan_busy;

static unsigned char dev_desc[18];
static unsigned char desc8[8];
static unsigned char hub_desc[16];
static unsigned char port_st[4];
static unsigned char conf_hdr[9];
static unsigned char conf_buf[USB_CONFIG_MAX];

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

static void indent(unsigned int depth)
{
	unsigned int i;
	for (i = 0; i < depth; i++)
		io_putstr("  ");
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

/* wPortStatus (little-endian) from the GET_PORT_STATUS buffer. */
static unsigned int port_status_word(void)
{
	return (unsigned int)port_st[0] | ((unsigned int)port_st[1] << 8);
}

int usb_control_in(unsigned int ctrl, unsigned int addr, unsigned int mps,
		   int low_speed, unsigned char rt, unsigned char req,
		   unsigned short value, unsigned short index,
		   unsigned char *buf, unsigned int len)
{
	unsigned char setup[8];

	if (!hcd)
		return -1;
	setup[0] = rt;
	setup[1] = req;
	setup[2] = (unsigned char)(value & 0xFF);
	setup[3] = (unsigned char)((value >> 8) & 0xFF);
	setup[4] = (unsigned char)(index & 0xFF);
	setup[5] = (unsigned char)((index >> 8) & 0xFF);
	setup[6] = (unsigned char)(len & 0xFF);
	setup[7] = (unsigned char)((len >> 8) & 0xFF);
	return hcd->control_transfer(ctrl, addr, mps, low_speed, setup, buf, len, 1);
}

int usb_control_out(unsigned int ctrl, unsigned int addr, unsigned int mps,
		    int low_speed, unsigned char rt, unsigned char req,
		    unsigned short value, unsigned short index)
{
	unsigned char setup[8];

	if (!hcd)
		return -1;
	setup[0] = rt;
	setup[1] = req;
	setup[2] = (unsigned char)(value & 0xFF);
	setup[3] = (unsigned char)((value >> 8) & 0xFF);
	setup[4] = (unsigned char)(index & 0xFF);
	setup[5] = (unsigned char)((index >> 8) & 0xFF);
	setup[6] = 0;
	setup[7] = 0;
	return hcd->control_transfer(ctrl, addr, mps, low_speed, setup, 0, 0, 0);
}

int usb_control_out_data(unsigned int ctrl, unsigned int addr,
			 unsigned int mps, int low_speed, unsigned char rt,
			 unsigned char req, unsigned short value,
			 unsigned short index, const unsigned char *buf,
			 unsigned int len)
{
	unsigned char setup[8];

	if (!hcd)
		return -1;
	setup[0] = rt;
	setup[1] = req;
	setup[2] = (unsigned char)(value & 0xFF);
	setup[3] = (unsigned char)((value >> 8) & 0xFF);
	setup[4] = (unsigned char)(index & 0xFF);
	setup[5] = (unsigned char)((index >> 8) & 0xFF);
	setup[6] = (unsigned char)(len & 0xFF);
	setup[7] = (unsigned char)((len >> 8) & 0xFF);
	return hcd->control_transfer(ctrl, addr, mps, low_speed, setup,
				     (unsigned char *)buf, len, 0);
}

/* Read the configuration descriptor and hand every interface with an
   interrupt-IN endpoint to the active probe. */
static void probe_config(unsigned int ctrl, unsigned int addr, unsigned int mps,
			 int low, unsigned int port, unsigned int vendor,
			 unsigned int product)
{
	struct usb_iface_info info;
	unsigned int total, i;
	int have_iface = 0;

	if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_STD,
			   REQ_GET_DESCRIPTOR,
			   (unsigned short)(DESC_CONFIG << 8), 0,
			   conf_hdr, 9) < 0) {
		dbg("usb:   config descriptor header failed\n");
		return;
	}
	total = (unsigned int)conf_hdr[2] | ((unsigned int)conf_hdr[3] << 8);
	if (total < 9)
		return;
	if (total > USB_CONFIG_MAX)
		total = USB_CONFIG_MAX;
	if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_STD,
			   REQ_GET_DESCRIPTOR,
			   (unsigned short)(DESC_CONFIG << 8), 0,
			   conf_buf, total) < 0) {
		dbg("usb:   config descriptor read failed\n");
		return;
	}
	dbg_dec("usb:   config descriptor len ", total);

	info.ctrl = ctrl;
	info.addr = addr;
	info.mps = mps;
	info.low_speed = low;
	info.port = port;
	info.vendor = vendor;
	info.product = product;
	info.config_value = conf_buf[5];
	info.cls = 0;
	info.subcls = 0;
	info.proto = 0;
	info.iface_num = 0;
	info.ep_addr = 0;
	info.ep_mps = 0;
	info.ep_interval = 0;

	i = 0;
	while (i + 2 <= total) {
		unsigned int blen = conf_buf[i];
		unsigned int btype = conf_buf[i + 1];

		if (blen < 2 || i + blen > total)
			break;

		if (btype == DESC_INTERFACE && blen >= 9) {
			if (have_iface && info.ep_addr) {
				dbg("usb:   interface has interrupt-IN ep\n");
				if (current_probe)
					current_probe(&info);
			}
			info.cls = conf_buf[i + 5];
			info.subcls = conf_buf[i + 6];
			info.proto = conf_buf[i + 7];
			info.iface_num = conf_buf[i + 2];
			if (print_devices) {
				io_putstr("    Interface ");
				put_uint(info.iface_num);
				io_putstr(": ");
				put_hex(info.cls, 2);
				io_putchar('/');
				put_hex(info.subcls, 2);
				io_putchar('/');
				put_hex(info.proto, 2);
				io_putchar('\n');
			} else {
				dbg_dec("usb:   iface class ", info.cls);
				dbg_dec("usb:   iface subclass ", info.subcls);
				dbg_dec("usb:   iface protocol ", info.proto);
			}
			info.ep_addr = 0;
			info.ep_mps = 0;
			info.ep_interval = 0;
			have_iface = 1;
		} else if (btype == DESC_ENDPOINT && blen >= 7 && have_iface) {
			unsigned int epa = conf_buf[i + 2];
			unsigned int attr = conf_buf[i + 3] & 3u;
			unsigned int mps = ((unsigned int)conf_buf[i + 4] |
					    ((unsigned int)conf_buf[i + 5] << 8)) & 0x7FFu;
			if (print_devices) {
				io_putstr("      Endpoint 0x");
				put_hex(epa, 2);
				io_putstr(attr == 3u ? " interrupt" : " other");
				io_putstr(", mps ");
				put_uint(mps);
				io_putchar('\n');
			}
			if (!info.ep_addr && (epa & 0x80u) && attr == 3u) {
				info.ep_addr = epa;
				info.ep_mps = mps;
				info.ep_interval = conf_buf[i + 6];
				dbg_dec("usb:   interrupt-IN endpoint ", epa & 0x0Fu);
			}
		}
		i += blen;
	}
	if (have_iface && info.ep_addr && current_probe)
		current_probe(&info);
}

static void enumerate_hub(unsigned int ctrl, unsigned int addr, unsigned int mps,
			  int low, unsigned int port, unsigned int depth);
static void enumerate_device(unsigned int ctrl, unsigned int addr,
			     unsigned int mps, int low, unsigned int port,
			     unsigned int depth);
static void enumerate_new_device(unsigned int ctrl, unsigned int port, int low,
				 unsigned int depth);

static void enumerate_device(unsigned int ctrl, unsigned int addr,
			     unsigned int mps, int low, unsigned int port,
			     unsigned int depth)
{
	unsigned int vendor, product, cls, subcls, proto;

	if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_STD,
			   REQ_GET_DESCRIPTOR,
			   (unsigned short)(DESC_DEVICE << 8), 0,
			   dev_desc, 18) < 0) {
		dbg("usb:   device descriptor read failed\n");
		if (print_devices) {
			indent(depth);
			io_putstr("(device descriptor read failed)\n");
		}
		return;
	}

	vendor  = dev_desc[8]  | ((unsigned int)dev_desc[9]  << 8);
	product = dev_desc[10] | ((unsigned int)dev_desc[11] << 8);
	cls     = dev_desc[4];
	subcls  = dev_desc[5];
	proto   = dev_desc[6];

	if (device_probe)
		device_probe(ctrl, addr, mps, low, port, vendor, product);

	if (print_devices) {
		indent(depth);
		io_putstr("Bus ");
		put_uint(ctrl + 1);
		io_putstr(" Device ");
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
	}

	if ((current_probe || print_devices) && cls != 0x09)
		probe_config(ctrl, addr, mps, low, port, vendor, product);

	if (cls == 0x09)
		enumerate_hub(ctrl, addr, mps, low, port, depth + 1);
}

static void enumerate_hub(unsigned int ctrl, unsigned int addr, unsigned int mps,
			  int low, unsigned int port, unsigned int depth)
{
	unsigned int nports, p;

	if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_CLS,
			   REQ_GET_DESCRIPTOR,
			   (unsigned short)(DESC_HUB << 8), 0, hub_desc,
			   sizeof(hub_desc)) < 0)
		return;

	nports = hub_desc[2];
	if (nports > 8)
		nports = 8;

	for (p = 1; p <= nports; p++) {
		int low2;

		if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_OTH,
				   REQ_GET_STATUS, 0, (unsigned short)p,
				   port_st, 4) < 0)
			continue;
		if (!(port_status_word() & PSTAT_CONNECT))
			continue;

		/* Power and reset the downstream port. */
		usb_control_out(ctrl, addr, mps, low, RT_HOST2DEV_OTH,
				REQ_SET_FEATURE, FEAT_PORT_POWER,
				(unsigned short)p);
		if (usb_control_out(ctrl, addr, mps, low, RT_HOST2DEV_OTH,
				    REQ_SET_FEATURE, FEAT_PORT_RESET,
				    (unsigned short)p) < 0)
			continue;
		msleep(30);

		usb_control_out(ctrl, addr, mps, low, RT_HOST2DEV_OTH,
				REQ_CLEAR_FEATURE, FEAT_C_PORT_CONNECTION,
				(unsigned short)p);

		/* Re-read status to learn the child's speed. */
		low2 = 0;
		if (usb_control_in(ctrl, addr, mps, low, RT_DEV2HOST_OTH,
				   REQ_GET_STATUS, 0, (unsigned short)p,
				   port_st, 4) >= 0) {
			if (port_status_word() & PSTAT_LOW_SPEED)
				low2 = 1;
		}

		enumerate_new_device(ctrl, port, low2, depth);
	}
}

static void enumerate_new_device(unsigned int ctrl, unsigned int port, int low,
				 unsigned int depth)
{
	unsigned int mps;
	unsigned int addr = next_addr[ctrl];

	if (addr > 127) {
		if (print_devices) {
			indent(depth);
			io_putstr("(too many devices)\n");
		}
		return;
	}

	/* First 8 bytes at address 0 (learn max packet size). */
	if (usb_control_in(ctrl, 0, 8, low, RT_DEV2HOST_STD,
			   REQ_GET_DESCRIPTOR,
			   (unsigned short)(DESC_DEVICE << 8), 0,
			   desc8, 8) < 0) {
		dbg("usb:   no response at address 0\n");
		if (print_devices) {
			indent(depth);
			io_putstr("(no response at address 0)\n");
		}
		return;
	}

	mps = desc8[7];
	if (mps == 0)
		mps = 8;

	/* Assign an address. */
	if (usb_control_out(ctrl, 0, mps, low, RT_HOST2DEV_STD,
			    REQ_SET_ADDRESS, (unsigned short)addr, 0) < 0) {
		dbg("usb:   SET_ADDRESS failed\n");
		if (print_devices) {
			indent(depth);
			io_putstr("(SET_ADDRESS failed)\n");
		}
		return;
	}
	dbg_dec("usb:   enumerated device, addr ", addr);
	msleep(5);

	next_addr[ctrl] = addr + 1;
	enumerate_device(ctrl, addr, mps, low, port, depth);
}

int usb_hcd_init(void)
{
	if (hcd)
		return 0;
	if (usb_uhci_ops.init() > 0) {
		hcd = &usb_uhci_ops;
		return 0;
	}
	if (usb_ohci_ops.init() > 0) {
		hcd = &usb_ohci_ops;
		return 0;
	}
	if (usb_xhci_ops.init() > 0) {
		hcd = &usb_xhci_ops;
		return 0;
	}
	return -1;
}

const char *usb_hcd_name(void)
{
	return hcd ? hcd->name : "none";
}

unsigned int usb_generation(void)
{
	return generation;
}

int usb_port_connected(unsigned int ctrl, unsigned int port)
{
	if (!hcd)
		return 0;
	return hcd->port_connected(ctrl, port);
}

int usb_int_in_start(unsigned int ctrl, unsigned int slot, unsigned int addr,
		     unsigned int ep, unsigned int mps, int low_speed)
{
	if (!hcd || slot >= USB_HCD_INT_SLOTS)
		return -1;
	return hcd->int_in_start(ctrl, slot, addr, ep, mps, low_speed);
}

int usb_int_in_poll(unsigned int slot, unsigned char *buf, unsigned int len)
{
	if (!hcd || slot >= USB_HCD_INT_SLOTS)
		return 0;
	return hcd->int_in_poll(slot, buf, len);
}

void usb_int_in_stop(unsigned int slot)
{
	if (hcd && slot < USB_HCD_INT_SLOTS)
		hcd->int_in_stop(slot);
}

void usb_register_device_probe(usb_device_probe_fn fn)
{
	device_probe = fn;
}

int usb_bulk_out(unsigned int ctrl, unsigned int addr, unsigned int ep,
		 unsigned int mps, const unsigned char *buf,
		 unsigned int len)
{
	if (!hcd || !hcd->bulk_out)
		return -1;
	return hcd->bulk_out(ctrl, addr, ep, mps, buf, len);
}

int usb_bulk_in(unsigned int ctrl, unsigned int addr, unsigned int ep,
		unsigned int mps, unsigned char *buf, unsigned int len)
{
	if (!hcd || !hcd->bulk_in)
		return -1;
	return hcd->bulk_in(ctrl, addr, ep, mps, buf, len);
}

int usb_scan(usb_probe_fn probe)
{
	unsigned int c, p;

	if (scan_busy)
		return -2;
	if (usb_hcd_init() != 0)
		return -1;

	scan_busy = 1;
	generation++;
	current_probe = probe;
	print_devices = (probe == 0);

	for (c = 0; c < hcd->count() && c < USB_HCD_MAX_CTRL; c++) {
		next_addr[c] = 1;

		if (print_devices) {
			unsigned int bus, dev, func, base;
			int mmio;

			hcd->info(c, &bus, &dev, &func, &base, &mmio);
			io_putstr(hcd->name);
			io_putstr(" Host Controller (PCI ");
			put_hex(bus, 2);
			io_putchar(':');
			put_hex(dev, 2);
			io_putchar('.');
			put_hex(func, 1);
			io_putstr(mmio ? ", MMIO 0x" : ", I/O 0x");
			put_hex(base, mmio ? 8 : 4);
			io_putstr(")\n");
		}

		for (p = 0; p < hcd->port_count(c); p++) {
			unsigned int conn = (unsigned int)hcd->port_connected(c, p);

			dbg_port(c, p, conn,
				 (unsigned int)hcd->port_low_speed(c, p));
			if (!conn)
				continue;
			hcd->port_take_change(c, p);   /* clear stale CSC */
			if (hcd->port_reset(c, p) != 0) {
				dbg("usb:   port reset failed\n");
				continue;
			}
			dbg("usb:   port reset ok\n");
			enumerate_new_device(c, p, hcd->port_low_speed(c, p), 1);
		}
	}

	current_probe = 0;
	print_devices = 0;
	scan_busy = 0;
	return 0;
}

int usb_hotplug_poll(usb_probe_fn probe)
{
	static unsigned int last;
	unsigned int now, c, p;
	int n = 0;

	if (!hcd || scan_busy)
		return 0;

	now = get_ticks_ms();
	if (now - last < HOTPLUG_MS)
		return 0;
	last = now;

	scan_busy = 1;
	current_probe = probe;
	for (c = 0; c < hcd->count() && c < USB_HCD_MAX_CTRL; c++) {
		for (p = 0; p < hcd->port_count(c); p++) {
			if (!hcd->port_take_change(c, p))
				continue;
			if (!hcd->port_connected(c, p))
				continue;
			if (hcd->port_reset(c, p) != 0)
				continue;
			enumerate_new_device(c, p, hcd->port_low_speed(c, p), 1);
			n++;
		}
	}
	current_probe = 0;
	scan_busy = 0;
	return n;
}

void usb_enumerate_and_print(void)
{
	int r = usb_scan(0);

	if (r == -1)
		io_putstr("lsusb: no USB host controller found\n");
	else if (r == -2)
		io_putstr("lsusb: a USB scan is already running\n");
}
