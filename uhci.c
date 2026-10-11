/* UHCI (USB 1.1) host controller driver.
   PCI discovery, register access, frame-list / queue-head / transfer-
   descriptor schedule, root-hub port control, polling control transfers and
   one interrupt-IN endpoint (the mouse).  Polling based (no IRQ); the PIC
   slave (and thus the UHCI IRQ) is masked, so we poll TD status.

   Only the first controller instance is driven (the frame list lives at a
   fixed low-memory address); among several PCI functions we pick the one
   whose root port has a device attached. */

#include "pci.h"
#include "serial.h"
#include "timer.h"
#include "uhci.h"
#include "usb_hcd.h"

/* Register offsets from the I/O base. */
#define USBCMD       0x00
#define USBSTS       0x02
#define USBINTR      0x04
#define USBFLBASEADD 0x08
#define USBPORTSC1   0x10
#define USBPORTSC2   0x12

/* USBCMD bits. */
#define USBCMD_RS      0x0001
#define USBCMD_HCRESET 0x0002
#define USBCMD_GRESET  0x0004
#define USBCMD_CF      0x0040
#define USBCMD_MAXP    0x0080

/* USBSTS bits. */
#define USBSTS_HCH 0x0020

/* PORTSC bits. */
#define USBPORTSC_CCS  0x0001
#define USBPORTSC_CSC  0x0002
#define USBPORTSC_PE   0x0004
#define USBPORTSC_PEC  0x0008
#define USBPORTSC_LSDA 0x0100
#define USBPORTSC_PR   0x0200

/* TD status bits. */
#define TD_ACTIVE   0x00800000u
#define TD_LS       0x04000000u
#define TD_CERR_3   0x18000000u
#define TD_NAK      0x00080000u
#define TD_ERR_MASK 0x007E0000u
#define TD_ERR_NO_NAK (TD_ERR_MASK & ~TD_NAK)
#define TD_ACTLEN   0x000007FFu

/* TD token PIDs (UHCI-encoded). */
#define TD_PID_SETUP 0x2Du
#define TD_PID_IN    0x69u
#define TD_PID_OUT   0xE1u

/* PCI: base class 0x0C (serial), subclass 0x03 (USB), prog.if 0x00 (UHCI). */
#define PCI_USB_CLASS 0x0C03u

struct uhci_td {
	unsigned int link;
	volatile unsigned int status;
	unsigned int token;
	unsigned int buffer;
} __attribute__((aligned(16)));

struct uhci_qh {
	unsigned int link;
	unsigned int element;
} __attribute__((aligned(16)));

#define UHCI_NUM_TD 16

/* Frame list lives in low memory (below the kernel at 0x7E00) to avoid
   bloating the kernel's BSS past its link-time budget (see kernel.ld). It
   must be 4 KB aligned; 0x3000 is free after boot (see AGENTS.md memory
   map). */
#define UHCI_FRAME_LIST_ADDR 0x3000u

static struct uhci_td tds[UHCI_NUM_TD];
static struct uhci_qh qh;

static unsigned char setup_buf[8];

/* Interrupt-IN endpoints (slot 0 = mouse, slot 1 = keyboard): one TD and
   one scheduled QH each, both serviced every frame. */
struct uhci_int_ep {
	struct uhci_td td;
	struct uhci_qh qh;
	unsigned char buf[8] __attribute__((aligned(16)));
	unsigned int addr, ep, mps;
	unsigned int toggle;
	int low;
	int armed;
};
static struct uhci_int_ep int_eps[USB_HCD_INT_SLOTS];

static unsigned int io_base;
static unsigned int pci_bus, pci_dev, pci_func;
static int found;
static int ctl_busy;

static unsigned short reg_readw(unsigned int reg)
{
	return port_inw((unsigned short)(io_base + reg));
}

static void reg_writew(unsigned int reg, unsigned short val)
{
	port_outw((unsigned short)(io_base + reg), val);
}

static void reg_writel(unsigned int reg, unsigned int val)
{
	port_outl((unsigned short)(io_base + reg), val);
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static void td_set(struct uhci_td *td, unsigned int link, unsigned int status,
		   unsigned int token, unsigned int buffer)
{
	td->link = link;
	td->status = status;
	td->token = token;
	td->buffer = buffer;
}

static unsigned int td_token(unsigned int pid, unsigned int addr,
			     unsigned int ep, unsigned int toggle,
			     unsigned int maxlen)
{
	unsigned int explen = (maxlen == 0) ? 0x7FFu
					    : ((maxlen - 1) & 0x7FFu);
	return (pid & 0xFFu) |
	       ((addr & 0x7Fu) << 8) |
	       ((ep & 0xFu) << 15) |
	       ((toggle & 1u) << 19) |
	       (explen << 21);
}

/* Bring the controller up: global + host reset, empty schedule (control QH
   followed by the interrupt QH), run state. */
static void reset_controller(void)
{
	unsigned int *fl = (unsigned int *)UHCI_FRAME_LIST_ADDR;
	unsigned int i;

	reg_writew(USBCMD, USBCMD_GRESET);
	delay_ms(50);
	reg_writew(USBCMD, 0);
	delay_ms(1);

	reg_writew(USBCMD, USBCMD_HCRESET);
	delay_ms(50);
	reg_writew(USBCMD, 0);
	delay_ms(1);

	for (i = 0; i < 1024; i++)
		fl[i] = (unsigned int)&qh | 0x2u;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		int_eps[i].qh.element = 0x1u;
		int_eps[i].qh.link = (i + 1 < USB_HCD_INT_SLOTS)
			? ((unsigned int)&int_eps[i + 1].qh | 0x2u) : 0x1u;
		int_eps[i].td.status = 0;
		int_eps[i].armed = 0;
	}
	qh.link = (unsigned int)&int_eps[0].qh | 0x2u;
	qh.element = 0x1u;

	reg_writel(USBFLBASEADD, UHCI_FRAME_LIST_ADDR);
	reg_writew(USBINTR, 0);
	reg_writew(USBSTS, 0x3Fu);
	reg_writew(USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);
}

static int uhci_init(void)
{
	unsigned int bus, dev, func;
	unsigned int first_io = 0, first_bus = 0, first_dev = 0, first_func = 0;
	int have_first = 0;

	if (found)
		return 1;

	for (bus = 0; bus < 256 && !found; bus++) {
		for (dev = 0; dev < 32; dev++) {
			for (func = 0; func < 8; func++) {
				unsigned int id = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int class_rev, cmd, bar, io, st;

				if (id == 0xFFFFFFFFu || id == 0)
					continue;
				class_rev = pci_cfg_read32(bus, dev, func, 0x08);
				if ((class_rev >> 16) != PCI_USB_CLASS)
					continue;
				if (((class_rev >> 8) & 0xFFu) != 0x00u)
					continue;   /* not UHCI */

				/* Enable I/O space + bus master. */
				cmd = pci_cfg_read32(bus, dev, func, 0x04);
				pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0005u);

				/* Disable legacy SMI emulation. */
				pci_cfg_write16(bus, dev, func, 0xC0, 0);

				/* The I/O BAR is BAR4 on UHCI controllers. */
				bar = pci_cfg_read32(bus, dev, func, 0x20);
				if (!(bar & 0x1u))
					continue;
				io = bar & 0xFFFFFFFCu;
				if (!io)
					continue;   /* BAR not assigned */

				/* Prefer a controller with something plugged in. */
				st = (unsigned int)port_inw((unsigned short)(io + USBPORTSC1)) |
				     (unsigned int)port_inw((unsigned short)(io + USBPORTSC2));
				if (st & USBPORTSC_CCS) {
					io_base = io;
					pci_bus = bus;
					pci_dev = dev;
					pci_func = func;
					found = 1;
					break;
				}
				if (!have_first) {
					have_first = 1;
					first_io = io;
					first_bus = bus;
					first_dev = dev;
					first_func = func;
				}
			}
			if (found)
				break;
		}
	}

	if (!found && have_first) {
		io_base = first_io;
		pci_bus = first_bus;
		pci_dev = first_dev;
		pci_func = first_func;
		found = 1;
	}
	if (!found)
		return 0;

	reset_controller();

	serial_puts(SERIAL_COM1, "uhci: controller at ");
	serial_putdec(SERIAL_COM1, pci_bus);
	serial_puts(SERIAL_COM1, ":");
	serial_putdec(SERIAL_COM1, pci_dev);
	serial_puts(SERIAL_COM1, ".");
	serial_putdec(SERIAL_COM1, pci_func);
	serial_puts(SERIAL_COM1, ", 2 ports\n");
	return 1;
}

static unsigned int uhci_count(void)
{
	return found ? 1u : 0u;
}

static void uhci_info(unsigned int ctrl, unsigned int *bus, unsigned int *dev,
		      unsigned int *func, unsigned int *base, int *mmio)
{
	(void)ctrl;
	*bus = pci_bus;
	*dev = pci_dev;
	*func = pci_func;
	*base = io_base;
	*mmio = 0;
}

static unsigned int uhci_port_count(unsigned int ctrl)
{
	(void)ctrl;
	return 2;
}

static int uhci_port_connected(unsigned int ctrl, unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	(void)ctrl;
	return (reg_readw(reg) & USBPORTSC_CCS) ? 1 : 0;
}

static int uhci_port_low_speed(unsigned int ctrl, unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	(void)ctrl;
	return (reg_readw(reg) & USBPORTSC_LSDA) ? 1 : 0;
}

static int uhci_port_take_change(unsigned int ctrl, unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	(void)ctrl;
	if (!(reg_readw(reg) & USBPORTSC_CSC))
		return 0;
	reg_writew(reg, USBPORTSC_CSC);   /* write-1-to-clear */
	return 1;
}

static int uhci_port_reset(unsigned int ctrl, unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	unsigned int st;
	(void)ctrl;

	/* Assert reset (this also resets the attached device). */
	st = reg_readw(reg);
	reg_writew(reg, (unsigned short)((st & ~USBPORTSC_PE) | USBPORTSC_PR));
	delay_ms(50);

	/* Deassert reset and clear the connect/enable change bits. */
	st = reg_readw(reg);
	reg_writew(reg, (unsigned short)((st & ~USBPORTSC_PR) |
					 USBPORTSC_CSC | USBPORTSC_PEC));
	delay_ms(20);

	/* Enable the port: QEMU (and some hardware) require software to
	   set PE explicitly after a reset. */
	st = reg_readw(reg);
	reg_writew(reg, (unsigned short)(st | USBPORTSC_PE));
	delay_ms(10);

	st = reg_readw(reg);
	if (!(st & USBPORTSC_PE))
		return -1;
	return 0;
}

static int control_transfer(unsigned int dev_addr, unsigned int mps,
			    int low_speed, const unsigned char *setup,
			    unsigned char *data, unsigned int len, int dir_in)
{
	unsigned int i, ntd = 0, n_data = 0;
	unsigned int base = TD_ACTIVE | TD_CERR_3 | (low_speed ? TD_LS : 0);
	unsigned int t0;

	if (!found)
		return -1;
	if (mps == 0 || mps > 64)
		mps = 64;

	for (i = 0; i < 8; i++)
		setup_buf[i] = setup[i];

	/* SETUP stage (8 bytes, DATA0). */
	td_set(&tds[ntd++], 0, base, td_token(TD_PID_SETUP, dev_addr, 0, 0, 8),
	       (unsigned int)setup_buf);

	/* DATA stage (split into max-packet-sized TDs, DATA1/DATA0/...). */
	if (len > 0) {
		unsigned int n = (len + mps - 1) / mps;
		unsigned int offset = 0;
		unsigned int toggle = 1;
		for (i = 0; i < n; i++) {
			unsigned int chunk = len - offset;
			if (chunk > mps)
				chunk = mps;
			td_set(&tds[ntd++], 0, base,
			       td_token(dir_in ? TD_PID_IN : TD_PID_OUT,
					dev_addr, 0, toggle, chunk),
			       (unsigned int)(data + offset));
			offset += chunk;
			toggle ^= 1u;
		}
		n_data = n;
	}

	/* STATUS stage (0 bytes, DATA1). */
	td_set(&tds[ntd++], 0, base,
	       td_token(dir_in ? TD_PID_OUT : TD_PID_IN, dev_addr, 0, 1, 0), 0);

	/* Chain the TDs. */
	for (i = 0; i + 1 < ntd; i++)
		tds[i].link = (unsigned int)&tds[i + 1];
	tds[ntd - 1].link = 0x1u;

	qh.element = (unsigned int)&tds[0];

	/* Run and poll until all TDs complete. */
	reg_writew(USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);

	t0 = get_ticks_ms();
	for (;;) {
		int done = 1;
		for (i = 0; i < ntd; i++) {
			unsigned int st = tds[i].status;
			if (st & TD_ACTIVE) {
				done = 0;
				continue;
			}
			if (st & TD_ERR_MASK) {
				qh.element = 0x1u;
				return -1;
			}
		}
		if (done)
			break;
		if (get_ticks_ms() - t0 > 2000) {
			qh.element = 0x1u;
			return -1;
		}
	}

	qh.element = 0x1u;

	{
		unsigned int total = 0;
		for (i = 0; i < n_data; i++) {
			unsigned int st = tds[1 + i].status;
			total += ((st & TD_ACTLEN) + 1) & TD_ACTLEN;
		}
		return (int)total;
	}
}

static int uhci_control_transfer(unsigned int ctrl, unsigned int addr,
				 unsigned int mps, int low_speed,
				 const unsigned char *setup,
				 unsigned char *data, unsigned int len,
				 int dir_in)
{
	int r;

	(void)ctrl;
	if (ctl_busy)
		return -1;
	ctl_busy = 1;
	r = control_transfer(addr, mps, low_speed, setup, data, len, dir_in);
	ctl_busy = 0;
	return r;
}

/* Queue the armed interrupt TD of one slot.  Only call when it is retired. */
static void int_rearm(struct uhci_int_ep *ep)
{
	ep->td.link = 0x1u;
	ep->td.buffer = (unsigned int)ep->buf;
	ep->td.token = td_token(TD_PID_IN, ep->addr, ep->ep, ep->toggle, ep->mps);
	ep->td.status = TD_ACTIVE | TD_CERR_3 | (ep->low ? TD_LS : 0);
	ep->qh.element = (unsigned int)&ep->td;
}

static int uhci_int_in_start(unsigned int ctrl, unsigned int slot,
			     unsigned int addr, unsigned int ep_num,
			     unsigned int mps, int low_speed)
{
	struct uhci_int_ep *ep;

	(void)ctrl;
	if (!found || slot >= USB_HCD_INT_SLOTS)
		return -1;
	ep = &int_eps[slot];

	/* Drop any old TD from the schedule, then write the new one. */
	ep->armed = 0;
	ep->qh.element = 0x1u;
	delay_ms(2);

	ep->addr = addr;
	ep->ep = ep_num;
	ep->mps = mps ? mps : 8;
	ep->low = low_speed;
	ep->toggle = 0;   /* first report is DATA0 */
	int_rearm(ep);
	ep->armed = 1;
	return 0;
}

static int uhci_int_in_poll(unsigned int slot, unsigned char *buf,
			    unsigned int len)
{
	struct uhci_int_ep *ep;
	unsigned int st, n, i;

	if (!found || slot >= USB_HCD_INT_SLOTS)
		return 0;
	ep = &int_eps[slot];
	if (!ep->armed)
		return 0;

	st = ep->td.status;
	if (st & TD_ACTIVE)
		return 0;

	if (st & TD_ERR_NO_NAK) {
		/* Report the error but stay armed (toggle preserved). */
		int_rearm(ep);
		return -1;
	}
	if (st & TD_NAK) {
		/* Some controllers retire a NAKed TD; just retry. */
		int_rearm(ep);
		return 0;
	}

	n = ((st & TD_ACTLEN) + 1) & TD_ACTLEN;
	if (n > len)
		n = len;
	for (i = 0; i < n; i++)
		buf[i] = ep->buf[i];

	ep->toggle ^= 1u;
	int_rearm(ep);
	return (int)n;
}

static void uhci_int_in_stop(unsigned int slot)
{
	struct uhci_int_ep *ep;

	if (slot >= USB_HCD_INT_SLOTS)
		return;
	ep = &int_eps[slot];
	ep->armed = 0;
	if (found)
		ep->qh.element = 0x1u;
}

const struct usb_hcd_ops usb_uhci_ops = {
	"UHCI",
	uhci_init,
	uhci_count,
	uhci_info,
	uhci_control_transfer,
	uhci_port_count,
	uhci_port_connected,
	uhci_port_low_speed,
	uhci_port_reset,
	uhci_port_take_change,
	uhci_int_in_start,
	uhci_int_in_poll,
	uhci_int_in_stop,
};
