/* UHCI (USB 1.1) host controller driver.
   PCI discovery, register access, frame-list / queue-head / transfer-
   descriptor schedule, and root-hub port control. Polling based (no IRQ);
   the PIC slave (and thus the UHCI IRQ) is masked, so we poll TD status. */

#include "pci.h"
#include "timer.h"
#include "uhci.h"

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
#define TD_ERR_MASK 0x007E0000u
#define TD_ACTLEN   0x000007FFu

/* TD token PIDs (UHCI-encoded). */
#define TD_PID_SETUP 0x2Du
#define TD_PID_IN    0x69u
#define TD_PID_OUT   0xE1u

/* PCI: base class 0x0C (serial), subclass 0x03 (USB). */
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
   bloating the kernel's BSS past the 0x20000 linker budget. It must be
   4 KB aligned; 0x3000 is free after boot (see AGENTS.md memory map). */
#define UHCI_FRAME_LIST_ADDR 0x3000u

static struct uhci_td tds[UHCI_NUM_TD];
static struct uhci_qh qh;

static unsigned char setup_buf[8];

static unsigned int io_base;
static unsigned int pci_bus, pci_dev, pci_func;
static int found;

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

/* Stop the controller (clear RS) and wait for it to actually halt. */
static void uhci_halt(void)
{
	unsigned int t0;
	reg_writew(USBCMD, USBCMD_CF | USBCMD_MAXP);
	t0 = get_ticks_ms();
	while (!(reg_readw(USBSTS) & USBSTS_HCH)) {
		if (get_ticks_ms() - t0 > 100)
			break;
	}
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

	qh.link = 0x1u;
	qh.element = 0x1u;

	reg_writel(USBFLBASEADD, UHCI_FRAME_LIST_ADDR);
	reg_writew(USBINTR, 0);
	reg_writew(USBSTS, 0x3Fu);
	reg_writew(USBCMD, USBCMD_CF | USBCMD_MAXP);
}

int uhci_init(void)
{
	unsigned int bus, dev;

	found = 0;
	io_base = 0;

	for (bus = 0; bus < 256 && !found; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int class_rev, cmd, bar;
			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			if ((class_rev >> 16) != PCI_USB_CLASS)
				continue;
			if (((class_rev >> 8) & 0xFFu) != 0x00u)
				continue;

			/* Enable I/O space + bus master. */
			cmd = pci_cfg_read32(bus, dev, 0, 0x04);
			pci_cfg_write32(bus, dev, 0, 0x04, cmd | 0x0005u);

			/* Disable legacy SMI emulation. */
			pci_cfg_write16(bus, dev, 0, 0xC0, 0);

			bar = pci_cfg_read32(bus, dev, 0, 0x20);
			if (!(bar & 0x1u))
				continue;
			io_base = bar & 0xFFFFFFFCu;
			pci_bus = bus;
			pci_dev = dev;
			pci_func = 0;
			found = 1;
			break;
		}
	}

	if (!found)
		return -1;

	reset_controller();
	return 0;
}

void uhci_pci_info(unsigned int *bus, unsigned int *dev, unsigned int *func,
		   unsigned int *base)
{
	*bus = pci_bus;
	*dev = pci_dev;
	*func = pci_func;
	*base = io_base;
}

unsigned int uhci_root_port_count(void)
{
	return 2;
}

int uhci_root_port_connected(unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	return (reg_readw(reg) & USBPORTSC_CCS) ? 1 : 0;
}

int uhci_root_port_low_speed(unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	return (reg_readw(reg) & USBPORTSC_LSDA) ? 1 : 0;
}

int uhci_root_port_reset(unsigned int port)
{
	unsigned int reg = (port == 0) ? USBPORTSC1 : USBPORTSC2;
	unsigned int st;

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

int uhci_control_transfer(unsigned int dev_addr, unsigned int mps, int low_speed,
			  const unsigned char *setup, unsigned char *data,
			  unsigned int len, int dir_in)
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
				uhci_halt();
				return -1;
			}
		}
		if (done)
			break;
		if (get_ticks_ms() - t0 > 2000) {
			uhci_halt();
			return -1;
		}
	}

	uhci_halt();

	{
		unsigned int total = 0;
		for (i = 0; i < n_data; i++) {
			unsigned int st = tds[1 + i].status;
			total += ((st & TD_ACTLEN) + 1) & TD_ACTLEN;
		}
		return (int)total;
	}
}
