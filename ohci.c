/* OHCI (USB 1.1) host controller driver.

   Memory-mapped (BAR0) controller with ED/TD lists in the identity-mapped
   kernel address space.  Polling based (no IRQ): control transfers run on
   the control list and are monitored by polling each TD's condition code;
   the mouse's interrupt-IN endpoint is a single always-armed TD linked into
   every HCCA interrupt-table slot (F = 0 means "every frame"), so the
   controller retries NAKs on its own and software only harvests reports.

   Up to four controller instances (MCP79 exposes four of them); each PCI
   function is one root hub with its own device address space. */

#include "ohci.h"
#include "usb_hcd.h"
#include "pci.h"
#include "paging.h"
#include "mm.h"
#include "serial.h"
#include "timer.h"

/* Register offsets. */
#define R_REVISION       0x00
#define R_CONTROL        0x04
#define R_COMMAND        0x08
#define R_INT_STATUS     0x0C
#define R_INT_ENABLE     0x10
#define R_INT_DISABLE    0x14
#define R_HCCA           0x18
#define R_PERIOD_CUR_ED  0x1C
#define R_CTRL_HEAD_ED   0x20
#define R_CTRL_CUR_ED    0x24
#define R_BULK_HEAD_ED   0x28
#define R_BULK_CUR_ED    0x2C
#define R_DONE_HEAD      0x30
#define R_FM_INTERVAL    0x34
#define R_FM_REMAINING   0x38
#define R_FM_NUMBER      0x3C
#define R_PERIODIC_START 0x40
#define R_LS_THRESHOLD   0x44
#define R_RH_DESC_A      0x48
#define R_RH_DESC_B      0x4C
#define R_RH_STATUS      0x50
#define R_RH_PORT_FIRST  0x54

/* R_CONTROL bits. */
#define CTL_PLE      0x00000004u
#define CTL_CLE      0x00000010u
#define CTL_BLE      0x00000020u
#define CTL_HCFS     0x000000C0u
#define HCFS_RESUME  0x00000040u
#define HCFS_OPER    0x00000080u

/* R_COMMAND bits. */
#define CMD_HCR 0x00000001u
#define CMD_CLF 0x00000002u

/* R_INT_STATUS bits. */
#define INTR_WDH 0x00000002u

/* R_RH_DESC_A bits. */
#define RHA_PSM    0x00000100u
#define RHA_NPS    0x00000200u
#define RHA_POTPGT 0xFF000000u
#define RHA_NDP    0x000000FFu

/* R_RH_STATUS bits. */
#define RHS_LPSC 0x00010000u

/* R_RH_DESC_B: PortPowerControlMask (which ports are individually
   switched when PSM is set). */
#define RHB_PPCM 0xFFFF0000u

/* Root hub port status bits. */
#define PS_CCS  0x00000001u
#define PS_PES  0x00000002u
#define PS_PRS  0x00000010u
#define PS_PPS  0x00000100u   /* port power (write 1 to switch power on) */
#define PS_LSDA 0x00000200u
#define PS_CSC  0x00010000u
#define PS_PESC 0x00020000u
#define PS_PRSC 0x00100000u
#define PS_WTC  (PS_CSC | PS_PESC | PS_PRSC)

/* Endpoint descriptor (ED) fields. */
#define ED_FA(a)  ((a) & 0x7Fu)
#define ED_EN(e)  (((e) & 0xFu) << 7)
#define ED_S      (1u << 13)
#define ED_MPS(m) (((m) & 0x7FFu) << 16)
#define ED_PTR_MASK 0xFFFFFFF0u
#define ED_H      0x00000001u   /* head pointer: halted */
#define ED_C      0x00000002u   /* head pointer: data-toggle carry */

/* Transfer descriptor (TD) fields. */
#define TD_R        (1u << 18)
#define TD_DP(d)    (((d) & 3u) << 19)
#define TD_DP_SETUP 0u
#define TD_DP_OUT   1u
#define TD_DP_IN    2u
/* Data toggle field (bits 24-25): 10b = DATA0, 11b = DATA1. */
#define TD_T_DATA0  0x02000000u
#define TD_T_DATA1  0x03000000u
#define TD_CC_SHIFT 28
#define TD_CC_MASK  0xF0000000u
#define TD_NOTACCESSED 0xFu

/* PCI: class 0x0C (serial), subclass 0x03 (USB), prog.if 0x10 (OHCI). */
#define PCI_USB_CLASS 0x0C03u

#define OHCI_MAX_TD 16

struct ohci_ed {
	unsigned int flags;
	unsigned int tail;
	unsigned int head;
	unsigned int next;
} __attribute__((aligned(16)));

struct ohci_td {
	unsigned int flags;
	unsigned int cbp;
	unsigned int next;
	unsigned int be;
} __attribute__((aligned(16)));

struct ohci_ctrl {
	volatile unsigned int *regs;
	unsigned int bus, dev, func, bar;
	unsigned int hcca;
	unsigned int ports;
	unsigned int potpgt;
	int nps;

	/* Interrupt-IN endpoints: slot 0 = mouse, slot 1 = keyboard.  All
	   slots stay linked into the periodic list; empty EDs are skipped
	   by the controller. */
	struct ohci_ed int_ed[USB_HCD_INT_SLOTS];
	struct ohci_td int_td[USB_HCD_INT_SLOTS];
	struct ohci_td int_dummy[USB_HCD_INT_SLOTS];
	unsigned char int_buf[USB_HCD_INT_SLOTS][8] __attribute__((aligned(16)));
	unsigned int int_mps[USB_HCD_INT_SLOTS];
	unsigned int int_toggle[USB_HCD_INT_SLOTS];
	int int_armed[USB_HCD_INT_SLOTS];
};

static struct ohci_ctrl ctrls[USB_HCD_MAX_CTRL];
static unsigned int n_ctrls;

/* Control-transfer scratch: transfers are serialized, so one ED/TD set for
   all instances is enough.  A dummy TD anchors the tail of each queue (the
   controller stops when HeadP == TailP; the dummy is never processed). */
static struct ohci_ed ctrl_ed;
static struct ohci_td ctrl_tds[OHCI_MAX_TD];
static struct ohci_td ctrl_dummy;
static unsigned char setup_buf[8] __attribute__((aligned(16)));
static int ctl_busy;

static void ohci_int_in_stop(unsigned int slot);

static unsigned int rd(struct ohci_ctrl *c, unsigned int ofs)
{
	return c->regs[ofs >> 2];
}

static void wr(struct ohci_ctrl *c, unsigned int ofs, unsigned int val)
{
	c->regs[ofs >> 2] = val;
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static unsigned int port_reg(unsigned int port)
{
	return R_RH_PORT_FIRST + 4u * port;
}

static int ohci_setup_one(unsigned int bus, unsigned int dev, unsigned int func)
{
	struct ohci_ctrl *c;
	unsigned int bar, cmd, rha, t0, i;

	if (n_ctrls >= USB_HCD_MAX_CTRL)
		return -1;

	bar = pci_cfg_read32(bus, dev, func, 0x10);
	if (bar & 0x1u) {
		serial_puts(SERIAL_COM1, "ohci: BAR0 is not MMIO\n");
		return -1;                      /* register block must be MMIO */
	}
	bar &= 0xFFFFFFF0u;
	if (!bar) {
		serial_puts(SERIAL_COM1, "ohci: BAR0 is unassigned\n");
		return -1;
	}

	cmd = pci_cfg_read32(bus, dev, func, 0x04);
	pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0006u);  /* mem + bus master */

	/* Disable legacy SMI emulation (same idea as the UHCI driver). */
	pci_cfg_write16(bus, dev, func, 0xC0, 0);

	c = &ctrls[n_ctrls];
	c->regs = (volatile unsigned int *)bar;
	c->bus = bus;
	c->dev = dev;
	c->func = func;
	c->bar = bar;

	paging_map_mmio(bar, 0x1000);

	if (rd(c, R_REVISION) == 0xFFFFFFFFu || rd(c, R_REVISION) == 0) {
		serial_puts(SERIAL_COM1, "ohci: no register response (rev ");
		serial_puthex(SERIAL_COM1, rd(c, R_REVISION));
		serial_puts(SERIAL_COM1, ")\n");
		return -1;
	}

	c->hcca = mm_alloc_page();
	if (!c->hcca) {
		serial_puts(SERIAL_COM1, "ohci: no memory for HCCA\n");
		return -1;
	}
	for (i = 0; i < 1024; i++)
		((unsigned int *)c->hcca)[i] = 0;

	/* Periodic list: the interrupt EDs are chained and referenced from
	   every HCCA table slot (F = 0 means "every frame").  Idle queues
	   are empty (HeadP == TailP == dummy) but carry valid ED fields. */
	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		c->int_ed[i].flags = ED_FA(0) | ED_EN(0) | ED_MPS(8);
		c->int_ed[i].tail = (unsigned int)&c->int_dummy[i];
		c->int_ed[i].head = (unsigned int)&c->int_dummy[i];
		c->int_ed[i].next = (i + 1 < USB_HCD_INT_SLOTS)
			? (unsigned int)&c->int_ed[i + 1] : 0;
		c->int_dummy[i].flags = TD_NOTACCESSED << TD_CC_SHIFT;
		c->int_dummy[i].cbp = 0;
		c->int_dummy[i].next = 0;
		c->int_dummy[i].be = 0;
		c->int_armed[i] = 0;
	}
	for (i = 0; i < 32; i++)
		((unsigned int *)c->hcca)[i] = (unsigned int)&c->int_ed[0];

	/* Host controller reset. */
	wr(c, R_COMMAND, CMD_HCR);
	t0 = get_ticks_ms();
	while (rd(c, R_COMMAND) & CMD_HCR) {
		if (get_ticks_ms() - t0 > 100)
			break;
	}
	delay_ms(2);

	/* Standard frame timing (12 MHz / 1 kHz - 1) and list setup. */
	wr(c, R_FM_INTERVAL, (0x2778u << 16) | 0x2EDFu);
	wr(c, R_PERIODIC_START, 0x2A2Fu);
	wr(c, R_LS_THRESHOLD, 0x628u);
	wr(c, R_HCCA, c->hcca);
	wr(c, R_INT_DISABLE, 0xFFFFFFFFu);
	wr(c, R_INT_STATUS, 0xFFFFFFFFu);   /* clear pending */
	wr(c, R_DONE_HEAD, 0);

	/* Root hub: port count and power. */
	rha = rd(c, R_RH_DESC_A);
	c->ports = rha & RHA_NDP;
	if (c->ports > 15)
		c->ports = 15;
	if (c->ports == 0) {
		serial_puts(SERIAL_COM1, "ohci: no downstream ports\n");
		return -1;
	}
	c->nps = (rha & RHA_NPS) ? 1 : 0;
	c->potpgt = (rha & RHA_POTPGT) >> 24;

	if (!c->nps) {
		/* Power switching is implemented: mark every port as
		   individually switchable, raise global power and switch on
		   each port, then let the supply settle (POTPGT in 2 ms
		   units). */
		wr(c, R_RH_DESC_B, RHB_PPCM);
		wr(c, R_RH_STATUS, RHS_LPSC);
		for (i = 0; i < c->ports; i++)
			wr(c, port_reg(i), PS_PPS);
		delay_ms(c->potpgt * 2 + 20);
	} else {
		delay_ms(20);
	}

	/* Leave reset (spec: via USBRESUME) and enter operational with the
	   periodic + control lists enabled. */
	wr(c, R_CONTROL, HCFS_RESUME);
	delay_ms(20);
	wr(c, R_CONTROL, HCFS_OPER | CTL_PLE | CTL_CLE);

	n_ctrls++;

	{
		unsigned int conn = 0, pwr = 0, st;

		for (i = 0; i < c->ports; i++) {
			st = rd(c, port_reg(i));
			if (st & PS_CCS)
				conn++;
			if (st & PS_PPS)
				pwr++;
		}

		serial_puts(SERIAL_COM1, "ohci: controller at ");
		serial_putdec(SERIAL_COM1, bus);
		serial_puts(SERIAL_COM1, ":");
		serial_putdec(SERIAL_COM1, dev);
		serial_puts(SERIAL_COM1, ".");
		serial_putdec(SERIAL_COM1, func);
		serial_puts(SERIAL_COM1, " ports ");
		serial_putdec(SERIAL_COM1, c->ports);
		serial_puts(SERIAL_COM1, " psm ");
		serial_putdec(SERIAL_COM1, (rha & RHA_PSM) ? 1u : 0u);
		serial_puts(SERIAL_COM1, " nps ");
		serial_putdec(SERIAL_COM1, (unsigned int)c->nps);
		serial_puts(SERIAL_COM1, " potpgt ");
		serial_putdec(SERIAL_COM1, c->potpgt);
		serial_puts(SERIAL_COM1, " conn ");
		serial_putdec(SERIAL_COM1, conn);
		serial_puts(SERIAL_COM1, " pwr ");
		serial_putdec(SERIAL_COM1, pwr);
		serial_puts(SERIAL_COM1, "\n");
	}
	return 0;
}

static int ohci_init(void)
{
	unsigned int bus, dev, func;

	if (n_ctrls)
		return (int)n_ctrls;

	for (bus = 0; bus < 256 && n_ctrls < USB_HCD_MAX_CTRL; bus++) {
		for (dev = 0; dev < 32; dev++) {
			for (func = 0; func < 8; func++) {
				unsigned int id = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int class_rev;

				if (id == 0xFFFFFFFFu || id == 0)
					continue;
				class_rev = pci_cfg_read32(bus, dev, func, 0x08);
				if ((class_rev >> 16) != PCI_USB_CLASS)
					continue;
				if (((class_rev >> 8) & 0xFFu) != 0x10u)
					continue;   /* not OHCI */

				ohci_setup_one(bus, dev, func);
			}
		}
	}
	return (int)n_ctrls;
}

static unsigned int ohci_count(void)
{
	return n_ctrls;
}

static void ohci_info(unsigned int ctrl, unsigned int *bus, unsigned int *dev,
		      unsigned int *func, unsigned int *base, int *mmio)
{
	struct ohci_ctrl *c = &ctrls[ctrl];
	*bus = c->bus;
	*dev = c->dev;
	*func = c->func;
	*base = c->bar;
	*mmio = 1;
}

static unsigned int ohci_port_count(unsigned int ctrl)
{
	return ctrls[ctrl].ports;
}

static int ohci_port_connected(unsigned int ctrl, unsigned int port)
{
	return (rd(&ctrls[ctrl], port_reg(port)) & PS_CCS) ? 1 : 0;
}

static int ohci_port_low_speed(unsigned int ctrl, unsigned int port)
{
	return (rd(&ctrls[ctrl], port_reg(port)) & PS_LSDA) ? 1 : 0;
}

static int ohci_port_take_change(unsigned int ctrl, unsigned int port)
{
	struct ohci_ctrl *c = &ctrls[ctrl];
	unsigned int ofs = port_reg(port);

	if (!(rd(c, ofs) & PS_CSC))
		return 0;
	wr(c, ofs, PS_CSC);   /* write-1-to-clear */
	return 1;
}

static int ohci_port_reset(unsigned int ctrl, unsigned int port)
{
	struct ohci_ctrl *c = &ctrls[ctrl];
	unsigned int ofs = port_reg(port);
	unsigned int t0, st;

	if (!(rd(c, ofs) & PS_CCS))
		return -1;

	/* Clear stale change bits. */
	wr(c, ofs, PS_WTC);

	/* Assert reset; the controller clears PRS when the reset finishes
	   (at least 10 ms for the device to see it). */
	wr(c, ofs, PS_PRS);
	t0 = get_ticks_ms();
	for (;;) {
		if (!(rd(c, ofs) & PS_PRS))
			break;
		if (get_ticks_ms() - t0 > 200)
			return -1;
	}
	delay_ms(10);

	/* Acknowledge reset-complete and make sure the port is enabled. */
	wr(c, ofs, PS_PRSC | PS_CSC | PS_PESC);
	wr(c, ofs, PS_PES);

	st = rd(c, ofs);
	if (!(st & PS_CCS) || !(st & PS_PES))
		return -1;
	return 0;
}

static void td_set(struct ohci_td *td, unsigned int flags, unsigned int cbp,
		   unsigned int be)
{
	td->flags = flags | (TD_NOTACCESSED << TD_CC_SHIFT);
	td->cbp = cbp;
	td->next = 0;
	td->be = be;
}

static int ohci_transfer(struct ohci_ctrl *c, unsigned int addr,
			 unsigned int mps, int low,
			 const unsigned char *setup, unsigned char *data,
			 unsigned int len, int dir_in)
{
	unsigned int i, n = 0, n_data = 0, t0;

	if (mps == 0 || mps > 64)
		mps = 64;

	for (i = 0; i < 8; i++)
		setup_buf[i] = setup[i];

	/* SETUP stage (DATA0). */
	td_set(&ctrl_tds[n++], TD_DP(TD_DP_SETUP) | TD_T_DATA0,
	       (unsigned int)setup_buf, (unsigned int)setup_buf + 7);

	/* DATA stage: one TD for the whole buffer, DATA1 (like Linux; the
	   controller splits it into MPS-sized packets itself). */
	if (len > 0) {
		td_set(&ctrl_tds[n++],
		       TD_R | TD_DP(dir_in ? TD_DP_IN : TD_DP_OUT) |
		       TD_T_DATA1,
		       (unsigned int)data, (unsigned int)(data + len - 1));
		n_data = 1;
	}

	/* STATUS stage (DATA1, opposite direction, no data). */
	td_set(&ctrl_tds[n++],
	       TD_DP(dir_in ? TD_DP_OUT : TD_DP_IN) | TD_T_DATA1, 0, 0);

	/* Chain the TDs and anchor the tail on the dummy TD. */
	for (i = 0; i + 1 < n; i++)
		ctrl_tds[i].next = (unsigned int)&ctrl_tds[i + 1];
	ctrl_tds[n - 1].next = (unsigned int)&ctrl_dummy;
	ctrl_dummy.flags = TD_NOTACCESSED << TD_CC_SHIFT;
	ctrl_dummy.cbp = 0;
	ctrl_dummy.next = 0;
	ctrl_dummy.be = 0;

	ctrl_ed.flags = ED_FA(addr) | ED_EN(0) | ED_MPS(mps) | (low ? ED_S : 0);
	ctrl_ed.tail = (unsigned int)&ctrl_dummy;
	ctrl_ed.head = (unsigned int)&ctrl_tds[0];
	ctrl_ed.next = 0;

	wr(c, R_CTRL_HEAD_ED, (unsigned int)&ctrl_ed);
	wr(c, R_COMMAND, CMD_CLF);

	/* Poll until every TD has left "not accessed".  A NAKed TD stays
	   untouched and is retried by the controller next frame. */
	t0 = get_ticks_ms();
	for (;;) {
		int done = 1, err = 0;

		for (i = 0; i < n; i++) {
			unsigned int cc = (ctrl_tds[i].flags & TD_CC_MASK)
					  >> TD_CC_SHIFT;
			if (cc == TD_NOTACCESSED) {
				done = 0;
				continue;
			}
			if (cc != 0) {
				err = 1;
				serial_puts(SERIAL_COM1, "ohci: xfer err td ");
				serial_putdec(SERIAL_COM1, i);
				serial_puts(SERIAL_COM1, " cc ");
				serial_putdec(SERIAL_COM1, cc);
				serial_putc(SERIAL_COM1, '\n');
			}
		}
		if (err) {
			ctrl_ed.head = (unsigned int)&ctrl_dummy;
			wr(c, R_CTRL_HEAD_ED, 0);
			return -1;
		}
		if (done)
			break;
		if (get_ticks_ms() - t0 > 2000) {
			serial_puts(SERIAL_COM1, "ohci: xfer timeout\n");
			ctrl_ed.head = (unsigned int)&ctrl_dummy;
			wr(c, R_CTRL_HEAD_ED, 0);
			return -1;
		}
	}
	ctrl_ed.head = (unsigned int)&ctrl_dummy;
	wr(c, R_CTRL_HEAD_ED, 0);

	/* Bytes actually received: the TD's CBP advances with the data
	   (the controller clears it to 0 when the whole buffer arrived). */
	if (len > 0 && dir_in && n_data == 1) {
		unsigned int cbp = ctrl_tds[1].cbp;

		if (cbp == 0)
			return (int)len;
		return (int)(cbp - (unsigned int)data);
	}
	return (int)len;
}

static int ohci_control_transfer(unsigned int ctrl, unsigned int addr,
				 unsigned int mps, int low_speed,
				 const unsigned char *setup,
				 unsigned char *data, unsigned int len,
				 int dir_in)
{
	int r;

	if (ctrl >= n_ctrls || ctl_busy)
		return -1;
	ctl_busy = 1;
	r = ohci_transfer(&ctrls[ctrl], addr, mps, low_speed, setup, data, len,
			  dir_in);
	ctl_busy = 0;
	return r;
}

/* Arm one interrupt TD of one slot: (re)write it active.  Only call with
   the TD retired.  The ED stays linked in the periodic chain. */
static void ohci_int_arm(struct ohci_ctrl *c, unsigned int slot)
{
	c->int_td[slot].flags = (TD_NOTACCESSED << TD_CC_SHIFT) | TD_R |
				TD_DP(TD_DP_IN) |
				(c->int_toggle[slot] ? TD_T_DATA1
						     : TD_T_DATA0);
	c->int_td[slot].cbp = (unsigned int)c->int_buf[slot];
	c->int_td[slot].next = (unsigned int)&c->int_dummy[slot];
	c->int_td[slot].be = (unsigned int)c->int_buf[slot] +
			     c->int_mps[slot] - 1;

	c->int_ed[slot].head = (unsigned int)&c->int_td[slot] |
			       (c->int_toggle[slot] ? ED_C : 0);
	c->int_ed[slot].tail = (unsigned int)&c->int_dummy[slot];
	/* int_ed[slot].next keeps the chain to the other slots. */

	c->int_armed[slot] = 1;
}

static int ohci_int_in_start(unsigned int ctrl, unsigned int slot,
			     unsigned int addr, unsigned int ep,
			     unsigned int mps, int low_speed)
{
	struct ohci_ctrl *c;

	if (ctrl >= n_ctrls || slot >= USB_HCD_INT_SLOTS)
		return -1;
	c = &ctrls[ctrl];

	ohci_int_in_stop(slot);

	c->int_mps[slot] = mps ? mps : 8;
	c->int_ed[slot].flags = ED_FA(addr) | ED_EN(ep) |
				ED_MPS(c->int_mps[slot]) |
				(low_speed ? ED_S : 0);
	c->int_ed[slot].tail = (unsigned int)&c->int_dummy[slot];
	c->int_ed[slot].head = (unsigned int)&c->int_dummy[slot];
	c->int_toggle[slot] = 0;   /* first report is DATA0 */
	ohci_int_arm(c, slot);
	return 0;
}

static int ohci_int_in_poll(unsigned int slot, unsigned char *buf,
			    unsigned int len)
{
	struct ohci_ctrl *c = 0;
	unsigned int i, cc, n;

	if (slot >= USB_HCD_INT_SLOTS)
		return 0;
	for (i = 0; i < n_ctrls; i++) {
		if (ctrls[i].int_armed[slot]) {
			c = &ctrls[i];
			break;
		}
	}
	if (!c)
		return 0;

	cc = (c->int_td[slot].flags & TD_CC_MASK) >> TD_CC_SHIFT;
	if (cc == TD_NOTACCESSED) {
		if (c->int_ed[slot].head & ED_H) {
			/* Halted by the controller without a CC update:
			   clear it and stay armed. */
			ohci_int_arm(c, slot);
			return -1;
		}
		return 0;   /* still queued; the controller retries NAKs */
	}
	if (cc != 0) {
		/* Report the error but stay armed (toggle preserved). */
		ohci_int_arm(c, slot);
		return -1;
	}

	n = c->int_td[slot].cbp
		? (c->int_td[slot].cbp - (unsigned int)c->int_buf[slot])
		: c->int_mps[slot];
	if (n > len)
		n = len;
	for (i = 0; i < n; i++)
		buf[i] = c->int_buf[slot][i];

	c->int_toggle[slot] ^= 1u;
	ohci_int_arm(c, slot);
	return (int)n;
}

static void ohci_int_in_stop(unsigned int slot)
{
	unsigned int i;

	if (slot >= USB_HCD_INT_SLOTS)
		return;
	for (i = 0; i < n_ctrls; i++) {
		struct ohci_ctrl *c = &ctrls[i];

		if (!c->int_armed[slot])
			continue;
		c->int_armed[slot] = 0;
		c->int_ed[slot].head = (unsigned int)&c->int_dummy[slot];
		c->int_ed[slot].tail = (unsigned int)&c->int_dummy[slot];
	}
}

const struct usb_hcd_ops usb_ohci_ops = {
	"OHCI",
	ohci_init,
	ohci_count,
	ohci_info,
	ohci_control_transfer,
	ohci_port_count,
	ohci_port_connected,
	ohci_port_low_speed,
	ohci_port_reset,
	ohci_port_take_change,
	ohci_int_in_start,
	ohci_int_in_poll,
	ohci_int_in_stop,
	0,
	0,
};
