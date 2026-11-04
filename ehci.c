/* EHCI (USB 2.0) host controller driver.
 *
 * Scope: high-speed devices on the root ports.  The EHCI controller is paired
 * with UHCI/OHCI companions on most chipsets; low/full-speed devices are left
 * to those controllers (the hardware routes them back when a port reset shows
 * a non-high-speed device).  usb.c enumerates every controller type, so a
 * high-speed USB Ethernet dongle on EHCI and a full-speed mouse/keyboard on a
 * companion both work.
 *
 * The schedule is polled (no IRQs), like the UHCI/OHCI drivers.  All control,
 * bulk and interrupt endpoints live in the asynchronous schedule: a circular
 * QH list with the control QH after the reclamation head.  Interrupt endpoints
 * are retried every microframe rather than through the periodic list; for a
 * handful of boot HID devices the extra NAK traffic is negligible and it keeps
 * the schedule simple.
 *
 * Descriptors (frame list, QHs, qTDs, bounce buffers) come from the page
 * allocator, so the driver does not grow the kernel BSS. */

#include "ehci.h"
#include "io.h"
#include "mm.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "timer.h"

#define EHCI_DEBUG 0

/* ---- capability / operational register offsets (bytes) ----------------- */
#define EHCI_CAP_CAPLENGTH	0x00
#define EHCI_CAP_HCSPARAMS	0x04
#define EHCI_CAP_HCCPARAMS	0x08

#define EHCI_OP_USBCMD		0x00
#define EHCI_OP_USBSTS		0x04
#define EHCI_OP_USBINTR		0x08
#define EHCI_OP_FRINDEX		0x0C
#define EHCI_OP_CTRLDSSEGMENT	0x10
#define EHCI_OP_PERIODICLIST	0x14
#define EHCI_OP_ASYNCLIST	0x18
#define EHCI_OP_CONFIGFLAG	0x40
#define EHCI_OP_PORTSC		0x44

/* USBCMD */
#define CMD_RUN			0x00000001u
#define CMD_RESET		0x00000002u
#define CMD_PSE			0x00000010u
#define CMD_ASE			0x00000020u
#define CMD_IAAD		0x00000040u
#define CMD_ITC_8		0x00080000u

/* USBSTS */
#define STS_INT			0x00000001u
#define STS_ERR			0x00000002u
#define STS_PCD			0x00000004u
#define STS_FLR			0x00000008u
#define STS_FATAL		0x00000010u
#define STS_IAA			0x00000020u
#define STS_HALT		0x00001000u
#define STS_RECL		0x00002000u
#define STS_PSS			0x00004000u
#define STS_ASS			0x00008000u

/* PORTSC */
#define PORT_CONNECT		0x00000001u
#define PORT_CSC		0x00000002u
#define PORT_PE			0x00000004u
#define PORT_PEC		0x00000008u
#define PORT_OC			0x00000010u
#define PORT_OCC		0x00000020u
#define PORT_RESUME		0x00000040u
#define PORT_SUSPEND		0x00000080u
#define PORT_RESET		0x00000100u
#define PORT_LS_MASK		0x00000C00u
#define PORT_POWER		0x00001000u
#define PORT_OWNER		0x00002000u
#define PORT_RWC		(PORT_CSC | PORT_PEC | PORT_OCC)
#define PORT_USB11(ps)		(((ps) & PORT_LS_MASK) == (1u << 10))

/* qTD token bits (EHCI 3.5.3). */
#define QTD_ACTIVE		0x00000080u
#define QTD_HALT		0x00000040u
#define QTD_DBE			0x00000020u
#define QTD_BABBLE		0x00000010u
#define QTD_XACT		0x00000008u
#define QTD_MMF			0x00000004u
#define QTD_PING		0x00000001u
#define QTD_ERRORS		(QTD_HALT | QTD_DBE | QTD_BABBLE | \
				 QTD_XACT | QTD_MMF)
#define QTD_TOGGLE		0x80000000u
#define QTD_PID_OUT		0u
#define QTD_PID_IN		1u
#define QTD_PID_SETUP		2u
#define QTD_CERR_3		(3u << 10)
#define QTD_LEN(tok)		(((tok) >> 16) & 0x7FFFu)

/* QH endpoint characteristics (EHCI 3.6). */
#define QH_DEVADDR(a)		((a) & 0x7Fu)
#define QH_ENDPT(e)		(((e) & 0x0Fu) << 8)
#define QH_EPS_HIGH		(2u << 12)
#define QH_DTC			(1u << 14)
#define QH_HEAD			(1u << 15)
#define QH_MPS(m)		(((m) & 0x7FFu) << 16)
#define QH_CTRL_EP		(1u << 27)
#define QH_RL(n)		(((n) & 7u) << 28)

/* QH endpoint capabilities. */
#define QH_MULT(n)		(((n) & 3u) << 30)

/* Link pointers: bit 0 terminates, bits 2:1 select the structure type. */
#define QH_LIST_END		1u
#define QH_NEXT(a)		(((a) & ~0x1Fu) | 0x2u)

#define EHCI_MAX_QH		(4 + USB_HCD_INT_SLOTS)
#define EHCI_QH_STRIDE		128
#define EHCI_QTD_STRIDE		64

struct ehci_qh {
	volatile unsigned int next;
	volatile unsigned int info1;
	volatile unsigned int info2;
	volatile unsigned int current;
	volatile unsigned int qtd_next;
	volatile unsigned int alt_next;
	volatile unsigned int token;
	volatile unsigned int buf[5];
	volatile unsigned int buf_hi[5];   /* 64-bit addressing extension */
} __attribute__((aligned(32)));

struct ehci_qtd {
	volatile unsigned int next;
	volatile unsigned int alt_next;
	volatile unsigned int token;
	volatile unsigned int buf[5];
	volatile unsigned int buf_hi[5];   /* 64-bit addressing extension */
} __attribute__((aligned(32)));

struct ehci_ctrl;

struct ehci_bulk {
	struct ehci_qh *qh;
	struct ehci_qtd *qtd;
	struct ehci_qtd *zqtd;      /* zero-length packet, bulk OUT only */
	unsigned int addr;
	unsigned int ep;
	unsigned int mps;
	unsigned int buf_len;
	unsigned int toggle;
	int armed;
	int sched;
	struct ehci_ctrl *owner;
};

struct ehci_int {
	struct ehci_qh *qh;
	struct ehci_qtd *qtd;
	unsigned char *buf;
	unsigned int mps;
	unsigned int buf_len;
	unsigned int toggle;
	int active;
	struct ehci_ctrl *owner;
};

struct ehci_ctrl {
	unsigned int base;
	unsigned int cap_len;
	unsigned int bus, dev, func;
	unsigned int n_ports;
	int ppc;
	int last_port;
	unsigned int hcs;
	unsigned int hcc;

	unsigned int frame_list;
	unsigned int desc_page;
	unsigned int data_page;

	struct ehci_qh *head;
	struct ehci_qh *ctl_qh;
	struct ehci_qh *bin_qh;
	struct ehci_qh *bout_qh;
	struct ehci_qh *int_qh[USB_HCD_INT_SLOTS];

	struct ehci_qtd *ctl_qtd[3];
	struct ehci_qtd *bin_qtd;
	struct ehci_qtd *bout_qtd;
	struct ehci_qtd *bout_zqtd;
	struct ehci_qtd *int_qtd[USB_HCD_INT_SLOTS];

	unsigned char *setup;
	unsigned char *rx_buf;
	unsigned char *int_buf[USB_HCD_INT_SLOTS];

	struct ehci_bulk bin;
	struct ehci_bulk bout;
	struct ehci_int ints[USB_HCD_INT_SLOTS];

	int ctl_busy;
	int recovering;
	int sched_on;
	int sched_refs;
	unsigned char addr_port[128];
};

static struct ehci_ctrl ctrls[USB_HCD_MAX_CTRL];
static unsigned int n_ctrls;
static struct ehci_int *slot_int[USB_HCD_INT_SLOTS];

static int ehci_start(struct ehci_ctrl *c);

/* ---- debug ------------------------------------------------------------- */
#if EHCI_DEBUG
static void dbg(const char *s)
{
	serial_puts(SERIAL_COM1, s);
}
#else
static void dbg(const char *s) { (void)s; }
#endif

static unsigned int rd32(unsigned int addr)
{
	return *(volatile unsigned int *)addr;
}

static void wr32(unsigned int addr, unsigned int val)
{
	*(volatile unsigned int *)addr = val;
}

static unsigned int op_reg(struct ehci_ctrl *c, unsigned int off)
{
	return c->base + c->cap_len + off;
}

/* The asynchronous schedule is only enabled while a transfer or an armed
 * interrupt endpoint needs it.  With it off the controller stops fetching
 * QHs and qTDs, which lets the package reach deep C-states while the
 * machine is idle; the port hotplug watcher only reads PORTSC and does not
 * need the schedule. */
static void ehci_sched_set(struct ehci_ctrl *c, int on)
{
	unsigned int v, t0;

	if (on == c->sched_on)
		return;
	v = rd32(op_reg(c, EHCI_OP_USBCMD));
	if (on) {
		wr32(op_reg(c, EHCI_OP_USBCMD), v | CMD_ASE);
		t0 = get_ticks_ms();
		while (!(rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_ASS)) {
			if (get_ticks_ms() - t0 > 10)
				break;
		}
	} else {
		/* Clear ASE and let the controller retire the current
		 * microframe; a pending IAA status must be acknowledged or the
		 * next enable can stall. */
		wr32(op_reg(c, EHCI_OP_USBCMD), v & ~CMD_ASE);
		t0 = get_ticks_ms();
		while (rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_ASS) {
			if (get_ticks_ms() - t0 > 10)
				break;
		}
		wr32(op_reg(c, EHCI_OP_USBSTS), STS_IAA);
	}
	c->sched_on = on;
}

static void ehci_sched_get(struct ehci_ctrl *c)
{
	c->sched_refs++;
	ehci_sched_set(c, 1);
}

static void ehci_sched_put(struct ehci_ctrl *c)
{
	if (c->sched_refs > 0)
		c->sched_refs--;
	if (!c->sched_refs)
		ehci_sched_set(c, 0);
}

static unsigned int port_reg(struct ehci_ctrl *c, unsigned int port)
{
	return c->base + c->cap_len + EHCI_OP_PORTSC + port * 4u;
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static void zero_region(unsigned int addr, unsigned int len)
{
	unsigned int *p = (unsigned int *)addr;
	while (len >= 4) {
		*p++ = 0;
		len -= 4;
	}
}

static void zero_ctrl(struct ehci_ctrl *c)
{
	unsigned char *p = (unsigned char *)c;
	unsigned int i;

	for (i = 0; i < sizeof(*c); i++)
		p[i] = 0;
}

/* ---- descriptor helpers ------------------------------------------------ */
static void qh_init(struct ehci_qh *qh)
{
	unsigned int i;

	qh->info1 = 0;
	qh->info2 = 0;
	qh->current = 0;
	qh->qtd_next = QH_LIST_END;
	qh->alt_next = QH_LIST_END;
	qh->token = QTD_HALT;
	for (i = 0; i < 5; i++) {
		qh->buf[i] = 0;
		qh->buf_hi[i] = 0;
	}
}

static void qh_configure(struct ehci_qh *qh, unsigned int addr,
			 unsigned int ep, unsigned int mps, int control)
{
	qh->info1 = QH_DEVADDR(addr) | QH_ENDPT(ep) | QH_EPS_HIGH |
		    QH_MPS(mps) | QH_RL(4);
	if (control)
		qh->info1 |= QH_CTRL_EP | QH_DTC;
	qh->info2 = QH_MULT(1);
}

static void qh_arm(struct ehci_qh *qh, struct ehci_qtd *qtd)
{
	__asm__ volatile ("" ::: "memory");
	qh->qtd_next = (unsigned int)qtd;
	qh->alt_next = QH_LIST_END;
	qh->token &= (QTD_TOGGLE | QTD_PING);
	__asm__ volatile ("" ::: "memory");
}

static void qh_disarm(struct ehci_qh *qh)
{
	qh->qtd_next = QH_LIST_END;
	qh->token = (qh->token & (QTD_TOGGLE | QTD_PING)) | QTD_HALT;
	__asm__ volatile ("" ::: "memory");
}

/* Fill one qTD.  Up to five 4 KB pages are described (all current callers
   are well below that); the C_PAGE field starts at zero. */
static void qtd_fill(struct ehci_qtd *qtd, const void *buf, unsigned int len,
		     unsigned int pid, int toggle)
{
	unsigned int addr = (unsigned int)buf;
	unsigned int left, chunk, i;

	qtd->next = QH_LIST_END;
	qtd->alt_next = QH_LIST_END;
	for (i = 0; i < 5; i++) {
		qtd->buf[i] = 0;
		qtd->buf_hi[i] = 0;
	}
	qtd->token = ((len & 0x7FFFu) << 16) |
		     ((pid & 3u) << 8) | QTD_CERR_3 | QTD_ACTIVE |
		     (toggle ? QTD_TOGGLE : 0);

	if (!len)
		return;

	qtd->buf[0] = addr;
	left = len;
	chunk = 0x1000u - (addr & 0xFFFu);
	if (chunk > left)
		chunk = left;
	left -= chunk;
	addr += chunk;

	i = 1;
	while (left && i < 5) {
		qtd->buf[i++] = addr & 0xFFFFF000u;
		chunk = (left > 0x1000u) ? 0x1000u : left;
		left -= chunk;
		addr += chunk;
	}
}

static int qtd_error(unsigned int token)
{
	return (token & QTD_ERRORS) != 0;
}

/* Wait for the qTD to retire.  If early is non-NULL and has halted, stop
   waiting immediately (a control data-stage STALL leaves the status qTD
   armed forever). */
static int qtd_wait(struct ehci_qtd *qtd, struct ehci_qtd *early,
		    unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (qtd->token & QTD_ACTIVE) {
		if (early && !(early->token & QTD_ACTIVE) &&
		    qtd_error(early->token))
			return -1;
		if (get_ticks_ms() - t0 >= ms)
			return -1;
		__asm__ volatile ("pause");
	}
	return 0;
}

/* ---- bulk endpoints ---------------------------------------------------- */
static void bulk_stop(struct ehci_bulk *b)
{
	if (b->qh)
		qh_disarm(b->qh);
	if (b->qtd)
		b->qtd->token = 0;
	if (b->zqtd)
		b->zqtd->token = 0;
	b->armed = 0;
	if (b->sched && b->owner) {
		ehci_sched_put(b->owner);
		b->sched = 0;
	}
}

static void bulk_reset(struct ehci_bulk *b)
{
	bulk_stop(b);
	b->addr = 0xFFFFFFFFu;
	b->ep = 0;
	b->mps = 0;
	b->buf_len = 0;
	b->toggle = 0;
}

static void bulk_configure(struct ehci_bulk *b, unsigned int addr,
			   unsigned int ep, unsigned int mps,
			   unsigned int buf_len)
{
	if (b->addr == addr && b->ep == ep && b->mps == mps)
		return;
	bulk_stop(b);
	b->addr = addr;
	b->ep = ep;
	b->mps = mps;
	b->buf_len = buf_len;
	b->toggle = 0;                  /* first packet is DATA0 */
	qh_init(b->qh);                 /* keeps the link pointer out of it */
	qh_configure(b->qh, addr, ep, mps, 0);
	b->qh->token = 0;               /* DATA0, not halted */
	b->armed = 0;
}

static void bulk_arm(struct ehci_bulk *b)
{
	if (!b->sched && b->owner) {
		ehci_sched_get(b->owner);
		b->sched = 1;
	}
	b->qh->token = (b->qh->token & QTD_PING) |
		       (b->toggle ? QTD_TOGGLE : 0);
	qh_arm(b->qh, b->qtd);
	b->armed = 1;
}

static int ehci_bulk_out(unsigned int ctrl, unsigned int addr,
			 unsigned int ep, unsigned int mps,
			 const unsigned char *buf, unsigned int len)
{
	struct ehci_ctrl *c;
	struct ehci_bulk *b;
	struct ehci_qtd *qtd, *zqtd, *last;
	unsigned int token, ztoken = 0;
	int zlp;

	if (ctrl >= n_ctrls || !buf || !len || len > 0x7FFFu)
		return -1;
	c = &ctrls[ctrl];
	b = &c->bout;
	if (!b->qh || !b->qtd || !b->zqtd)
		return -1;
	if (mps < 1)
		mps = 512;
	if (mps > 1024)
		mps = 1024;

	bulk_configure(b, addr, ep, mps, 0);
	bulk_stop(b);
	qtd = b->qtd;
	zqtd = b->zqtd;
	qtd_fill(qtd, buf, len, QTD_PID_OUT, 0);

	/* A bulk transfer that ends on a max-packet boundary needs a
	   zero-length packet to mark the end (USB 2.0 5.8.3). */
	zlp = (len % mps) == 0;
	if (zlp) {
		qtd_fill(zqtd, 0, 0, QTD_PID_OUT, 0);
		qtd->next = (unsigned int)zqtd;
		last = zqtd;
	} else {
		last = qtd;
	}
	bulk_arm(b);

	if (qtd_wait(last, qtd, 2000) < 0) {
		bulk_stop(b);
		return -1;
	}

	token = qtd->token;
	if (zlp)
		ztoken = last->token;
	bulk_stop(b);
	if ((token & QTD_ACTIVE) || qtd_error(token) ||
	    (zlp && ((ztoken & QTD_ACTIVE) || qtd_error(ztoken))))
		return -1;

	{
		unsigned int packets = (len + mps - 1) / mps;

		if (zlp)
			packets++;
		b->toggle ^= packets & 1u;
	}
	return 0;
}

/* One page of bounce buffer; the caller's buffer stays stable while the
   completed transfer is consumed (see usb_hcd.h). */
#define EHCI_RX_MAX	2048u

static int ehci_bulk_in(unsigned int ctrl, unsigned int addr,
			unsigned int ep, unsigned int mps,
			unsigned char *buf, unsigned int len)
{
	struct ehci_ctrl *c;
	struct ehci_bulk *b;
	unsigned int token, raw, packets, actual, i;

	if (ctrl >= n_ctrls || !buf || !len)
		return -1;
	c = &ctrls[ctrl];
	b = &c->bin;
	if (!b->qh || !b->qtd || !c->rx_buf)
		return -1;
	if (mps < 1)
		mps = 512;
	if (mps > 1024)
		mps = 1024;

	bulk_configure(b, addr, ep, mps, EHCI_RX_MAX);

	if (!b->armed) {
		qtd_fill(b->qtd, c->rx_buf, b->buf_len, QTD_PID_IN, 0);
		bulk_arm(b);
		return 0;
	}

	token = b->qtd->token;
	if (token & QTD_ACTIVE)
		return 0;

	b->armed = 0;
	if (qtd_error(token)) {
		bulk_stop(b);
		return -1;
	}

	raw = QTD_LEN(token) <= b->buf_len ?
		b->buf_len - QTD_LEN(token) : 0;
	actual = raw;
	if (actual > len)
		actual = len;
	for (i = 0; i < actual; i++)
		buf[i] = c->rx_buf[i];

	/* A bulk IN transfer ends with a short packet; a zero-length
	   packet also counts as one successful transaction. */
	packets = raw ? (raw + b->mps - 1) / b->mps : 1;
	b->toggle ^= packets & 1u;

	qh_disarm(b->qh);
	qtd_fill(b->qtd, c->rx_buf, b->buf_len, QTD_PID_IN, 0);
	bulk_arm(b);
	return (int)actual;
}

/* ---- interrupt-IN endpoints ------------------------------------------- */
static void int_arm(struct ehci_int *in)
{
	in->qh->token = (in->qh->token & QTD_PING) |
			(in->toggle ? QTD_TOGGLE : 0);
	qh_arm(in->qh, in->qtd);
}

static void ehci_int_in_stop(unsigned int slot)
{
	struct ehci_int *in;

	if (slot >= USB_HCD_INT_SLOTS)
		return;
	in = slot_int[slot];
	if (!in)
		return;
	{
		int was_active = in->active;

		in->active = 0;
		if (in->qh)
			qh_disarm(in->qh);
		if (in->qtd)
			in->qtd->token = 0;
		slot_int[slot] = 0;
		if (was_active && in->owner)
			ehci_sched_put(in->owner);
	}
}

static int ehci_int_in_start(unsigned int ctrl, unsigned int slot,
			     unsigned int addr, unsigned int ep,
			     unsigned int mps, int low_speed)
{
	struct ehci_ctrl *c;
	struct ehci_int *in;

	if (ctrl >= n_ctrls || slot >= USB_HCD_INT_SLOTS || low_speed)
		return -1;
	c = &ctrls[ctrl];
	in = &c->ints[slot];
	if (!in->qh || !in->qtd || !in->buf)
		return -1;

	ehci_int_in_stop(slot);

	in->mps = mps ? mps : 8;
	if (in->mps > 1024)
		in->mps = 1024;
	in->buf_len = in->mps > 64 ? 64 : in->mps;

	qh_init(in->qh);
	in->qh->info1 = QH_DEVADDR(addr) | QH_ENDPT(ep) | QH_EPS_HIGH |
			QH_MPS(in->mps) | QH_RL(4);
	in->qh->info2 = QH_MULT(1);
	in->qh->token = 0;
	in->toggle = 0;                 /* first report is DATA0 */

	qtd_fill(in->qtd, in->buf, in->buf_len, QTD_PID_IN, 0);
	int_arm(in);
	in->active = 1;
	slot_int[slot] = in;
	ehci_sched_get(c);
	return 0;
}

static int ehci_int_in_poll(unsigned int slot, unsigned char *buf,
			    unsigned int len)
{
	struct ehci_int *in;
	unsigned int token, raw, packets, actual, i;

	if (slot >= USB_HCD_INT_SLOTS)
		return 0;
	in = slot_int[slot];
	if (!in || !in->active)
		return 0;

	token = in->qtd->token;
	if (token & QTD_ACTIVE)
		return 0;

	if (qtd_error(token)) {
		qh_disarm(in->qh);
		qtd_fill(in->qtd, in->buf, in->buf_len, QTD_PID_IN, 0);
		int_arm(in);
		return -1;
	}

	raw = QTD_LEN(token) <= in->buf_len ?
		in->buf_len - QTD_LEN(token) : 0;
	actual = raw;
	if (actual > len)
		actual = len;
	for (i = 0; i < actual; i++)
		buf[i] = in->buf[i];

	packets = raw ? (raw + in->mps - 1) / in->mps : 1;
	in->toggle ^= packets & 1u;

	qh_disarm(in->qh);
	qtd_fill(in->qtd, in->buf, in->buf_len, QTD_PID_IN, 0);
	int_arm(in);
	return (int)actual;
}

/* ---- control transfers ------------------------------------------------- */
static int ehci_control(struct ehci_ctrl *c, unsigned int addr,
			unsigned int mps, int low_speed,
			const unsigned char *setup, unsigned char *data,
			unsigned int len, int dir_in)
{
	struct ehci_qh *qh;
	struct ehci_qtd *q0, *q1, *q2, *last, *early;
	unsigned int i, actual = 0, residual;

	(void)mps;
	if (!c->ctl_qh || !c->setup || !setup || c->ctl_busy || low_speed)
		return -1;
	if (len && !data)
		return -1;

	qh = c->ctl_qh;
	q0 = c->ctl_qtd[0];
	q1 = c->ctl_qtd[1];
	q2 = c->ctl_qtd[2];
	ehci_sched_get(c);
	c->ctl_busy = 1;

	for (i = 0; i < 8; i++)
		c->setup[i] = setup[i];

	qh_init(qh);
	qh_configure(qh, addr, 0, 64, 1);
	qh->token = 0;                  /* control toggles come from the qTDs */

	qtd_fill(q0, c->setup, 8, QTD_PID_SETUP, 0);
	if (len) {
		qtd_fill(q1, data, len,
			 dir_in ? QTD_PID_IN : QTD_PID_OUT, 1);
		qtd_fill(q2, 0, 0,
			 dir_in ? QTD_PID_OUT : QTD_PID_IN, 1);
		q0->next = (unsigned int)q1;
		q1->next = (unsigned int)q2;
		last = q2;
		early = q1;
	} else {
		qtd_fill(q2, 0, 0, QTD_PID_IN, 1);
		q0->next = (unsigned int)q2;
		last = q2;
		early = q0;
	}
	q2->next = QH_LIST_END;

	qh_arm(qh, q0);
	if (qtd_wait(last, early, 2000) < 0) {
		qh_disarm(qh);
		delay_ms(1);
		c->ctl_busy = 0;
		ehci_sched_put(c);
		return -1;
	}
	qh_disarm(qh);

	if (qtd_error(q0->token) || qtd_error(q2->token) ||
	    (len && qtd_error(q1->token))) {
		c->ctl_busy = 0;
		ehci_sched_put(c);
		return -1;
	}

	if (len && dir_in) {
		residual = QTD_LEN(q1->token);
		actual = residual >= len ? 0 : len - residual;
	} else if (len) {
		actual = len;
	}

	/* The device changes address after the status stage.  Remember which
	   root port it came from so a port reset can drop this controller's
	   bulk state without touching an unrelated port. */
	if (addr == 0 && setup[0] == 0x00 && setup[1] == 0x05) {
		unsigned int new_addr = (unsigned int)setup[2] |
					((unsigned int)setup[3] << 8);
		if (new_addr > 0 && new_addr < 128 && c->last_port >= 0)
			c->addr_port[new_addr] = (unsigned char)c->last_port;
	}

	c->ctl_busy = 0;
	ehci_sched_put(c);
	return (int)actual;
}

static int ehci_control_transfer(unsigned int ctrl, unsigned int addr,
				 unsigned int mps, int low_speed,
				 const unsigned char *setup,
				 unsigned char *data, unsigned int len,
				 int dir_in)
{
	if (ctrl >= n_ctrls)
		return -1;
	return ehci_control(&ctrls[ctrl], addr, mps, low_speed, setup, data,
			    len, dir_in);
}

/* ---- root hub ports ---------------------------------------------------- */
static void port_endpoints_reset(struct ehci_ctrl *c, unsigned int port)
{
	unsigned int i;

	if (c->bin.addr < 128 && c->addr_port[c->bin.addr] == port)
		bulk_reset(&c->bin);
	if (c->bout.addr < 128 && c->addr_port[c->bout.addr] == port)
		bulk_reset(&c->bout);
	for (i = 0; i < 128; i++)
		if (c->addr_port[i] == port)
			c->addr_port[i] = 0xFF;
}

static unsigned int ehci_port_count(unsigned int ctrl)
{
	return ctrl < n_ctrls ? ctrls[ctrl].n_ports : 0;
}

static int ehci_port_connected(unsigned int ctrl, unsigned int port)
{
	struct ehci_ctrl *c;
	unsigned int ps;

	if (ctrl >= n_ctrls || port >= ctrls[ctrl].n_ports)
		return 0;
	c = &ctrls[ctrl];
	ps = rd32(port_reg(c, port));
	return (ps & PORT_CONNECT) ? 1 : 0;
}

static int ehci_port_low_speed(unsigned int ctrl, unsigned int port)
{
	(void)ctrl;
	(void)port;
	return 0;                       /* EHCI only drives high-speed devices */
}

static int ehci_port_take_change(unsigned int ctrl, unsigned int port)
{
	struct ehci_ctrl *c;
	unsigned int ps;

	if (ctrl >= n_ctrls || port >= ctrls[ctrl].n_ports)
		return 0;
	c = &ctrls[ctrl];
	ps = rd32(port_reg(c, port));
	if (!(ps & PORT_CSC))
		return 0;
	wr32(port_reg(c, port),
	     (ps & ~PORT_RWC) | PORT_CSC | PORT_PEC | PORT_OCC);
	return 1;
}

static int ehci_port_reset_once(unsigned int ctrl, unsigned int port)
{
	struct ehci_ctrl *c;
	unsigned int ps, t0, try;

	if (ctrl >= n_ctrls || port >= ctrls[ctrl].n_ports)
		return -1;
	c = &ctrls[ctrl];

	/* A halted controller ignores port commands; restart it first. */
	if (rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_HALT) {
		serial_puts(SERIAL_COM1, "ehci: controller halted, restarting\n");
		if (ehci_start(c) < 0)
			return -1;
	}

	ps = rd32(port_reg(c, port));
	if (!(ps & PORT_CONNECT))
		return -1;

	/* Drop any reset/suspend state the firmware left behind.  The port
	   can be stuck with PR set; a single clear write is not enough on
	   some PCHs. */
	for (try = 0; try < 10 && (ps & PORT_RESET); try++) {
		wr32(port_reg(c, port),
		     (ps & ~(PORT_RWC | PORT_RESET | PORT_PE |
			     PORT_SUSPEND | PORT_RESUME)) |
		     PORT_CSC | PORT_PEC | PORT_OCC);
		delay_ms(5);
		ps = rd32(port_reg(c, port));
	}
	if ((ps & PORT_RESET) || !(ps & PORT_CONNECT))
		return -2;

	/* The port may still be owned by the companion controller.  A
	   low/full-speed device belongs there; a high-speed one can be taken
	   over by clearing PORT_OWNER (the controller may need more than one
	   write while the port is enabled). */
	if (ps & PORT_OWNER) {
		if (PORT_USB11(ps))
			return -1;
		for (try = 0; try < 8 && (ps & PORT_OWNER); try++) {
			wr32(port_reg(c, port),
			     (ps & ~(PORT_RWC | PORT_OWNER | PORT_RESET |
				     PORT_PE | PORT_SUSPEND | PORT_RESUME)) |
			     PORT_CSC | PORT_PEC | PORT_OCC);
			delay_ms(5);
			ps = rd32(port_reg(c, port));
		}
		if ((ps & PORT_OWNER) || !(ps & PORT_CONNECT))
			return -1;
	}

	port_endpoints_reset(c, port);
	c->last_port = (int)port;

	/* Clear stale change bits, then assert reset.  PED is cleared while
	   reset is asserted (like Linux's SetPortFeature(RESET)). */
	wr32(port_reg(c, port),
	     (ps & ~(PORT_RWC | PORT_PE | PORT_SUSPEND | PORT_RESUME)) |
	     PORT_CSC | PORT_PEC | PORT_OCC | PORT_RESET);
	delay_ms(50);

	/* EHCI expects software to end the reset, but some controllers hold
	   PR set while the device is still chirping.  Give it up to 200 ms to
	   self-clear before forcing the write. */
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 200) {
		if (!(rd32(port_reg(c, port)) & PORT_RESET))
			break;
		delay_ms(1);
	}

	/* Force completion if the controller still holds PR; retry because
	   the first write can land while the reset is still in progress. */
	for (try = 0; try < 20; try++) {
		ps = rd32(port_reg(c, port));
		if (!(ps & PORT_RESET))
			break;
		wr32(port_reg(c, port),
		     (ps & ~(PORT_RWC | PORT_RESET | PORT_PE |
			     PORT_SUSPEND | PORT_RESUME)) |
		     PORT_CSC | PORT_PEC | PORT_OCC);
		delay_ms(10);
	}
	if (ps & PORT_RESET) {
		serial_puts(SERIAL_COM1, "ehci: reset stuck ");
		serial_putdec(SERIAL_COM1, c->bus);
		serial_putc(SERIAL_COM1, ':');
		serial_putdec(SERIAL_COM1, c->dev);
		serial_putc(SERIAL_COM1, '.');
		serial_putdec(SERIAL_COM1, c->func);
		serial_puts(SERIAL_COM1, " port ");
		serial_putdec(SERIAL_COM1, port + 1);
		serial_puts(SERIAL_COM1, " ps ");
		serial_puthex(SERIAL_COM1, ps);
		serial_putc(SERIAL_COM1, '\n');
		return -2;
	}

	t0 = get_ticks_ms();
	while (!(rd32(port_reg(c, port)) & PORT_PE)) {
		if (get_ticks_ms() - t0 > 200)
			break;
	}
	ps = rd32(port_reg(c, port));

	if (!(ps & PORT_PE)) {
		if (ps & PORT_OWNER)
			return -1;
		if (PORT_USB11(ps)) {
			/* Low/full speed: make sure the companion owns it. */
			wr32(port_reg(c, port),
			     (ps & ~(PORT_RWC | PORT_PE)) | PORT_OWNER |
			     PORT_CSC | PORT_PEC | PORT_OCC);
			return -1;
		}
		serial_puts(SERIAL_COM1, "ehci: reset no PED ");
		serial_puthex(SERIAL_COM1, ps);
		serial_putc(SERIAL_COM1, '\n');
		return -2;
	}
	if (ps & PORT_OWNER)
		return -1;
	return 0;
}

/* A stuck HS port reset on some Intel PCHs only completes after the whole
   EHCI controller is reset.  Retry the port reset once in that case. */
static int ehci_port_reset(unsigned int ctrl, unsigned int port)
{
	struct ehci_ctrl *c;
	int r;

	if (ctrl >= n_ctrls || port >= ctrls[ctrl].n_ports)
		return -1;
	c = &ctrls[ctrl];
	r = ehci_port_reset_once(ctrl, port);
	if (r == -2 && !c->recovering) {
		c->recovering = 1;
		serial_puts(SERIAL_COM1,
			    "ehci: port reset retry after controller reset\n");
		if (ehci_start(c) == 0)
			r = ehci_port_reset_once(ctrl, port);
		else
			r = -1;
		c->recovering = 0;
	}
	return r < 0 ? -1 : 0;
}

/* ---- schedule / controller setup -------------------------------------- */
static int ehci_sched_init(struct ehci_ctrl *c)
{
	unsigned char *desc = (unsigned char *)c->desc_page;
	unsigned int qtd_base = c->desc_page + EHCI_MAX_QH * EHCI_QH_STRIDE;
	struct ehci_qh *list[EHCI_MAX_QH];
	unsigned int i;

	c->head = (struct ehci_qh *)(desc + 0 * EHCI_QH_STRIDE);
	c->ctl_qh = (struct ehci_qh *)(desc + 1 * EHCI_QH_STRIDE);
	c->bin_qh = (struct ehci_qh *)(desc + 2 * EHCI_QH_STRIDE);
	c->bout_qh = (struct ehci_qh *)(desc + 3 * EHCI_QH_STRIDE);
	for (i = 0; i < USB_HCD_INT_SLOTS; i++)
		c->int_qh[i] = (struct ehci_qh *)(desc +
					(4 + i) * EHCI_QH_STRIDE);

	c->ctl_qtd[0] = (struct ehci_qtd *)(qtd_base + 0 * EHCI_QTD_STRIDE);
	c->ctl_qtd[1] = (struct ehci_qtd *)(qtd_base + 1 * EHCI_QTD_STRIDE);
	c->ctl_qtd[2] = (struct ehci_qtd *)(qtd_base + 2 * EHCI_QTD_STRIDE);
	c->bin_qtd = (struct ehci_qtd *)(qtd_base + 3 * EHCI_QTD_STRIDE);
	c->bout_qtd = (struct ehci_qtd *)(qtd_base + 4 * EHCI_QTD_STRIDE);
	c->bout_zqtd = (struct ehci_qtd *)(qtd_base + 11 * EHCI_QTD_STRIDE);
	for (i = 0; i < USB_HCD_INT_SLOTS; i++)
		c->int_qtd[i] = (struct ehci_qtd *)(qtd_base +
					(5 + i) * EHCI_QTD_STRIDE);

	c->rx_buf = (unsigned char *)c->data_page;
	c->setup = (unsigned char *)(c->data_page + 2048);
	for (i = 0; i < USB_HCD_INT_SLOTS; i++)
		c->int_buf[i] = (unsigned char *)(c->data_page + 2112 +
						  i * 64);

	for (i = 0; i < 1024; i++)
		wr32(c->frame_list + i * 4, QH_LIST_END);

	list[0] = c->head;
	list[1] = c->ctl_qh;
	list[2] = c->bin_qh;
	list[3] = c->bout_qh;
	for (i = 0; i < USB_HCD_INT_SLOTS; i++)
		list[4 + i] = c->int_qh[i];

	for (i = 0; i < EHCI_MAX_QH; i++) {
		qh_init(list[i]);
		list[i]->next = QH_NEXT((unsigned int)
					list[(i + 1) % EHCI_MAX_QH]);
	}
	c->head->info1 = QH_HEAD;
	c->head->token = QTD_HALT;

	c->bin.owner = c;
	c->bin.sched = 0;
	c->bout.owner = c;
	c->bout.sched = 0;
	c->bin.qh = c->bin_qh;
	c->bin.qtd = c->bin_qtd;
	c->bin.zqtd = 0;
	c->bin.addr = 0xFFFFFFFFu;
	c->bout.qh = c->bout_qh;
	c->bout.qtd = c->bout_qtd;
	c->bout.zqtd = c->bout_zqtd;
	c->bout.addr = 0xFFFFFFFFu;
	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		c->ints[i].qh = c->int_qh[i];
		c->ints[i].qtd = c->int_qtd[i];
		c->ints[i].buf = c->int_buf[i];
		c->ints[i].active = 0;
		c->ints[i].owner = c;
	}
	for (i = 0; i < 128; i++)
		c->addr_port[i] = 0xFF;
	c->last_port = -1;
	return 0;
}

static void ehci_halt(struct ehci_ctrl *c)
{
	unsigned int t0;

	wr32(op_reg(c, EHCI_OP_USBINTR), 0);
	wr32(op_reg(c, EHCI_OP_USBCMD), 0);
	t0 = get_ticks_ms();
	while (!(rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_HALT)) {
		if (get_ticks_ms() - t0 > 100)
			break;
	}
}

static int ehci_start(struct ehci_ctrl *c)
{
	unsigned int v, t0, i;

	/* Stop first, then reset.  HCRESET self-clears. */
	ehci_halt(c);
	wr32(op_reg(c, EHCI_OP_USBCMD), CMD_RESET);
	t0 = get_ticks_ms();
	while (rd32(op_reg(c, EHCI_OP_USBCMD)) & CMD_RESET) {
		if (get_ticks_ms() - t0 > 250) {
			dbg("ehci: reset timeout\n");
			return -1;
		}
	}
	delay_ms(2);

	if (c->hcc & 1u)                /* 64-bit addressing capable */
		wr32(op_reg(c, EHCI_OP_CTRLDSSEGMENT), 0);
	wr32(op_reg(c, EHCI_OP_PERIODICLIST), c->frame_list);
	wr32(op_reg(c, EHCI_OP_ASYNCLIST), (unsigned int)c->head);
	wr32(op_reg(c, EHCI_OP_USBINTR), 0);
	wr32(op_reg(c, EHCI_OP_USBSTS), 0x3Fu);

	c->sched_on = 0;
	c->sched_refs = 0;
	wr32(op_reg(c, EHCI_OP_USBCMD), CMD_RUN | CMD_ITC_8);
	__asm__ volatile ("" ::: "memory");

	t0 = get_ticks_ms();
	while (rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_HALT) {
		if (get_ticks_ms() - t0 > 100) {
			dbg("ehci: controller did not start\n");
			return -1;
		}
	}

	/* Take the root ports over from the companion controllers. */
	wr32(op_reg(c, EHCI_OP_CONFIGFLAG), 1);
	v = rd32(op_reg(c, EHCI_OP_CONFIGFLAG));
	if (!(v & 1u))
		serial_puts(SERIAL_COM1, "ehci: CONFIGFLAG did not stick\n");
	delay_ms(5);

	if (c->ppc) {
		for (i = 0; i < c->n_ports; i++) {
			v = rd32(port_reg(c, i));
			if (!(v & PORT_POWER))
				wr32(port_reg(c, i),
				     (v & ~PORT_RWC) | PORT_POWER);
		}
		delay_ms(100);
	}

	t0 = get_ticks_ms();
	while (!(rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_ASS)) {
		if (get_ticks_ms() - t0 > 100)
			break;
	}
	if (!(rd32(op_reg(c, EHCI_OP_USBSTS)) & STS_ASS))
		dbg("ehci: async schedule not active\n");

	return 0;
}

/* Best-effort EHCI legacy-support handshake: stop the firmware's SMIs from
   firing on port changes now that the kernel owns the controller. */
static void legacy_smi_disable(unsigned int bus, unsigned int dev,
			       unsigned int func, unsigned int hcc)
{
	unsigned int eecp = (hcc >> 8) & 0xFFu;
	unsigned int guard = 0;

	while (eecp >= 0x40 && guard++ < 16) {
		unsigned int cap = pci_cfg_read32(bus, dev, func, eecp);
		unsigned int id = cap & 0xFFu;
		unsigned int next = (cap >> 8) & 0xFFu;

		if (id == 1) {          /* EHCI Legacy Support */
			pci_cfg_write32(bus, dev, func, eecp,
					(cap | (1u << 24)) & ~(1u << 31));
			cap = pci_cfg_read32(bus, dev, func, eecp + 4);
			pci_cfg_write32(bus, dev, func, eecp + 4,
					cap & 0xFFFF0000u);
			return;
		}
		if (!next || next == eecp)
			return;
		eecp = next;
	}
}

static int ehci_setup_one(unsigned int bus, unsigned int dev,
			  unsigned int func, unsigned int bar)
{
	struct ehci_ctrl *c;
	unsigned int cap_len, hcs, hcc;

	if (n_ctrls >= USB_HCD_MAX_CTRL)
		return -1;
	c = &ctrls[n_ctrls];
	zero_ctrl(c);
	c->base = bar;
	c->bus = bus;
	c->dev = dev;
	c->func = func;

	paging_map_mmio(bar, 0x1000);

	if (rd32(bar) == 0xFFFFFFFFu)
		return -1;
	cap_len = rd32(bar + EHCI_CAP_CAPLENGTH) & 0xFFu;
	if (cap_len < 0x20 || cap_len > 0x100)
		return -1;
	hcs = rd32(bar + EHCI_CAP_HCSPARAMS);
	if (hcs == 0xFFFFFFFFu)
		return -1;
	c->cap_len = cap_len;
	c->n_ports = hcs & 0x0Fu;
	c->ppc = (hcs & 0x10u) ? 1 : 0;
	c->hcs = hcs;
	if (!c->n_ports) {
		dbg("ehci: no root ports\n");
		return -1;
	}
	hcc = rd32(bar + EHCI_CAP_HCCPARAMS);
	c->hcc = hcc;
	legacy_smi_disable(bus, dev, func, hcc);

	c->frame_list = mm_alloc_page();
	c->desc_page = mm_alloc_page();
	c->data_page = mm_alloc_page();
	if (!c->frame_list || !c->desc_page || !c->data_page) {
		dbg("ehci: out of pages\n");
		goto fail;
	}
	zero_region(c->frame_list, 4096);
	zero_region(c->desc_page, 4096);
	zero_region(c->data_page, 4096);

	ehci_sched_init(c);
	if (ehci_start(c) < 0)
		goto fail;

	serial_puts(SERIAL_COM1, "ehci: controller at ");
	serial_putdec(SERIAL_COM1, bus);
	serial_puts(SERIAL_COM1, ":");
	serial_putdec(SERIAL_COM1, dev);
	serial_puts(SERIAL_COM1, ".");
	serial_putdec(SERIAL_COM1, func);
	serial_puts(SERIAL_COM1, " ports ");
	serial_putdec(SERIAL_COM1, c->n_ports);
	serial_puts(SERIAL_COM1, "\n");

	n_ctrls++;
	return 0;

fail:
	ehci_halt(c);
	/* Give the shared ports back to the companion controllers. */
	wr32(op_reg(c, EHCI_OP_CONFIGFLAG), 0);
	if (c->frame_list)
		mm_free_page(c->frame_list);
	if (c->desc_page)
		mm_free_page(c->desc_page);
	if (c->data_page)
		mm_free_page(c->data_page);
	zero_ctrl(c);
	return -1;
}

static int ehci_init(void)
{
	unsigned int bus, dev, func;

	if (n_ctrls)
		return (int)n_ctrls;

	for (bus = 0; bus < 256 && n_ctrls < USB_HCD_MAX_CTRL; bus++) {
		for (dev = 0; dev < 32; dev++) {
			for (func = 0; func < 8; func++) {
				unsigned int id = pci_cfg_read32(bus, dev, func,
								 0x00);
				unsigned int class_rev, bar, bar_hi, cmd;

				if (id == 0xFFFFFFFFu || id == 0)
					continue;
				class_rev = pci_cfg_read32(bus, dev, func,
							   0x08);
				if ((class_rev >> 16) != 0x0C03u)
					continue;
				if (((class_rev >> 8) & 0xFFu) != 0x20u)
					continue;   /* not EHCI */

				bar = pci_cfg_read32(bus, dev, func, 0x10);
				if (bar & 1u)
					continue;   /* register block is MMIO */
				bar_hi = 0;
				if (((bar >> 1) & 3u) == 2u)
					bar_hi = pci_cfg_read32(bus, dev, func,
								0x14);
				bar &= 0xFFFFFFF0u;
				if (!bar || bar_hi)
					continue;

				cmd = pci_cfg_read32(bus, dev, func, 0x04);
				pci_cfg_write32(bus, dev, func, 0x04,
						cmd | 0x0006u);

				ehci_setup_one(bus, dev, func, bar);
			}
		}
	}
	return (int)n_ctrls;
}

static unsigned int ehci_count(void)
{
	return n_ctrls;
}

static void ehci_info(unsigned int ctrl, unsigned int *bus, unsigned int *dev,
		      unsigned int *func, unsigned int *base, int *mmio)
{
	struct ehci_ctrl *c = &ctrls[ctrl];

	*bus = c->bus;
	*dev = c->dev;
	*func = c->func;
	*base = c->base;
	*mmio = 1;
}

/* Raw PORTSC for `lsusb`; useful when a port is connected but owned by a
   companion (bit 13) or not powered (bit 12). */
static void ehci_print_ports(unsigned int ctrl)
{
	struct ehci_ctrl *c;
	unsigned int i, ps, eecp;

	if (ctrl >= n_ctrls)
		return;
	c = &ctrls[ctrl];
	io_putstr("    CMD 0x");
	io_puthex(rd32(op_reg(c, EHCI_OP_USBCMD)));
	io_putstr(" STS 0x");
	io_puthex(rd32(op_reg(c, EHCI_OP_USBSTS)));
	io_putchar('\n');
	io_putstr("    CF 0x");
	io_puthex(rd32(op_reg(c, EHCI_OP_CONFIGFLAG)));
	io_putstr(" HCC 0x");
	io_puthex(c->hcc);
	io_putstr(" HCS 0x");
	io_puthex(c->hcs);
	io_putchar('\n');
	eecp = (c->hcc >> 8) & 0xFFu;
	if (eecp >= 0x40) {
		unsigned int leg = pci_cfg_read32(c->bus, c->dev, c->func,
						 eecp);
		if ((leg & 0xFFu) == 1u) {
			io_putstr("    LEGSUP 0x");
			io_puthex(leg);
			io_putchar('\n');
		}
	}

	for (i = 0; i < c->n_ports; i++) {
		ps = rd32(port_reg(c, i));
		io_putstr("    Port ");
		if (i < 9)
			io_putchar((char)('1' + i));
		else
			io_putchar((char)('A' + i - 9));
		io_putstr(": 0x");
		io_puthex(ps);
		io_putchar('\n');
	}
}

const struct usb_hcd_ops usb_ehci_ops = {
	"EHCI",
	ehci_init,
	ehci_count,
	ehci_info,
	ehci_control_transfer,
	ehci_port_count,
	ehci_port_connected,
	ehci_port_low_speed,
	ehci_port_reset,
	ehci_port_take_change,
	ehci_int_in_start,
	ehci_int_in_poll,
	ehci_int_in_stop,
	ehci_bulk_out,
	ehci_bulk_in,
	ehci_print_ports,
};
