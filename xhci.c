/* xHCI (USB 3.0) host controller driver.
 *
 * Scope: one controller, USB2 root ports and devices plugged directly into
 * them.  The USB3 ports are left alone (devices there need SuperSpeed
 * resource/companion-descriptor handling that the HID stack does not need
 * yet).  Control transfers and one interrupt-IN endpoint per HID interface
 * are implemented, which is exactly what usbhid.c uses.
 *
 * The schedule is polled, like the rest of the kernel.  Rings and contexts
 * are page-allocated so the driver does not grow the kernel BSS into the
 * 0x7F000 wall. */

#include "mm.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"
#include "spinlock.h"
#include "timer.h"
#include "usb_hcd.h"
#include "xhci.h"

#define XHCI_DEBUG 0

/* ---- controller limits ------------------------------------------------- */
#define XHCI_MAX_SLOTS		4
#define XHCI_MAX_PORTS		32
#define XHCI_EVT_TRBS		256
#define XHCI_CMD_TRBS		32
#define XHCI_EP0_TRBS		16
#define XHCI_INT_TRBS		16
#define XHCI_BULK_TRBS		32

/* ---- capability / operational / runtime offsets (bytes) --------------- */
#define XHCI_CAP_HCSPARAMS1	0x04
#define XHCI_CAP_HCSPARAMS2	0x08
#define XHCI_CAP_HCCPARAMS1	0x10
#define XHCI_CAP_DBOFF		0x14
#define XHCI_CAP_RTSOFF		0x18

#define XHCI_OP_USBCMD		0x00
#define XHCI_OP_USBSTS		0x04
#define XHCI_OP_PAGESIZE	0x08
#define XHCI_OP_CRCR		0x18
#define XHCI_OP_DCBAAP		0x30
#define XHCI_OP_CONFIG		0x38
#define XHCI_OP_PORTSC		0x400

#define XHCI_RT_IMAN		0x20
#define XHCI_RT_IMOD		0x24
#define XHCI_RT_ERSTSZ		0x28
#define XHCI_RT_ERSTBA		0x30
#define XHCI_RT_ERDP		0x38

/* USBCMD / USBSTS */
#define XHCI_CMD_RS		0x0001u
#define XHCI_CMD_HCRST		0x0002u
#define XHCI_STS_HCH		0x0001u
#define XHCI_STS_CNR		0x0800u
#define XHCI_STS_HCE		0x1000u

/* PORTSC */
#define PORTSC_CCS		0x00000001u
#define PORTSC_PED		0x00000002u
#define PORTSC_PR		0x00000010u
#define PORTSC_PP		0x00000200u
#define PORTSC_SPEED_SHIFT	10
#define PORTSC_SPEED_MASK	0xFu
#define PORTSC_CSC		0x00020000u
#define PORTSC_PEC		0x00040000u
#define PORTSC_WRC		0x00080000u
#define PORTSC_OCC		0x00100000u
#define PORTSC_PRC		0x00200000u
#define PORTSC_PLC		0x00400000u
#define PORTSC_CEC		0x00800000u
#define PORTSC_RWC		(PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
				 PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | \
				 PORTSC_CEC | PORTSC_PED)

/* TRB types */
#define TRB_NORMAL		1
#define TRB_SETUP		2
#define TRB_DATA		3
#define TRB_STATUS		4
#define TRB_LINK		6
#define TRB_ENABLE_SLOT		9
#define TRB_DISABLE_SLOT	10
#define TRB_ADDR_DEV		11
#define TRB_CONFIG_EP		12
#define TRB_TRANSFER		32
#define TRB_COMPLETION		33

/* TRB control bits */
#define TRB_CYCLE		0x00000001u
#define TRB_TC			0x00000002u
#define TRB_ISP			0x00000004u
#define TRB_CHAIN		0x00000010u
#define TRB_IOC			0x00000020u
#define TRB_IDT			0x00000040u
#define TRB_BSR			0x00000200u
#define TRB_DIR_IN		0x00010000u
#define TRB_TYPE(t)		((t) << 10)

/* completion codes */
#define CC_SUCCESS		1
#define CC_SHORT_PACKET		13

/* slot/EP context flags and endpoint types */
#define SLOT_FLAG		0x00000001u
#define EP0_FLAG		0x00000002u
#define EP_CTRL			4
#define EP_BULK_OUT		2
#define EP_BULK_IN		6
#define EP_INT_IN		7

#define XHCI_CTRL_MPS		8

struct xhci_trb {
	unsigned int p0;
	unsigned int p1;
	unsigned int status;
	unsigned int control;
};

struct xhci_slot {
	int used;
	unsigned int slot_id;
	unsigned int addr;
	unsigned int port;
	unsigned int speed;
	unsigned int dc;	/* device context page */
	unsigned int ep0;	/* EP0 transfer ring page */
	unsigned int ep0_enq;
	unsigned int ep0_cycle;
	unsigned int ep_mask;	/* configured endpoint flags */
};

struct xhci_int {
	int active;
	unsigned int slot;	/* hardware slot id */
	unsigned int ep_id;	/* xHCI endpoint id (2n+dir) */
	unsigned int mps;
	unsigned int ring;
	unsigned int enq;
	unsigned int cycle;
	unsigned int trb_len;
	unsigned int pending_len;
	int pending;
	int error;
	unsigned char buf[8] __attribute__((aligned(16)));
};

struct xhci_bulk {
	int active;
	int armed;
	unsigned int slot;
	unsigned int ep;	/* endpoint number */
	unsigned int ep_id;
	unsigned int mps;
	unsigned int ring;
	unsigned int enq;
	unsigned int cycle;
	unsigned int buf;
	unsigned int len;
	unsigned int trb_len;
	unsigned int actual;
	int pending;
	int error;
};

static unsigned int mmio_base;
static unsigned int cap_len;
static unsigned int rt_off;
static unsigned int db_off;
static unsigned int max_slots;
static unsigned int max_ports;
static unsigned int ctx_size;
static unsigned int usb2_mask;
static unsigned int hcc_params;
static unsigned int pci_bus, pci_dev, pci_func;
static int found;
static int last_port = -1;

static unsigned int dcbaa;
static unsigned int input_ctx;
static unsigned int cmd_ring;
static unsigned int evt_ring;
static unsigned int erst;
static unsigned int cmd_enq;
static unsigned int cmd_cycle;
static unsigned int evt_idx;
static unsigned int evt_cycle;

static unsigned int cmd_busy;
static unsigned int cmd_done;
static unsigned int cmd_status;
static unsigned int cmd_slot;
static unsigned int cmd_trb_addr;

static unsigned int ctl_busy;
static unsigned int ctl_done;
static unsigned int ctl_slot;
static unsigned int ctl_status;
static unsigned int ctl_req_len;
static unsigned int ctl_actual;
static unsigned int ctl_data_seen;
static unsigned int ctl_setup_trb;
static unsigned int ctl_data_trb;
static unsigned int ctl_status_trb;

static struct xhci_bulk bulk_in;
static struct xhci_bulk bulk_out;
static unsigned int out_busy;
static unsigned int out_done;
static unsigned int out_status;
static unsigned int out_slot;
static unsigned int out_trb;

static struct xhci_slot slots[XHCI_MAX_SLOTS + 1];
static unsigned int addr_slot[128];
static unsigned int port_slot[XHCI_MAX_PORTS];
static struct xhci_int ints[USB_HCD_INT_SLOTS];

static spinlock_t xhci_lock;

/* ---- debug ------------------------------------------------------------- */
#if XHCI_DEBUG
static void dbg(const char *s)
{
	serial_puts(SERIAL_COM1, s);
}

static void dbg_hex(const char *s, unsigned int v)
{
	serial_puts(SERIAL_COM1, s);
	serial_puthex(SERIAL_COM1, v);
	serial_putc(SERIAL_COM1, '\n');
}

static void dbg_dec(const char *s, unsigned int v)
{
	serial_puts(SERIAL_COM1, s);
	serial_putdec(SERIAL_COM1, v);
	serial_putc(SERIAL_COM1, '\n');
}
#else
static void dbg(const char *s) { (void)s; }
static void dbg_hex(const char *s, unsigned int v) { (void)s; (void)v; }
static void dbg_dec(const char *s, unsigned int v) { (void)s; (void)v; }
#endif

/* ---- MMIO helpers ------------------------------------------------------ */
static unsigned int mmio_rd(unsigned int off)
{
	return *(volatile unsigned int *)(mmio_base + off);
}

static void mmio_wr(unsigned int off, unsigned int val)
{
	*(volatile unsigned int *)(mmio_base + off) = val;
}

static void mmio_wr64(unsigned int off, unsigned int lo)
{
	mmio_wr(off, lo);
	mmio_wr(off + 4, 0);
}

/* Normal RAM (DCBAA, ERST, scratchpad array) is not MMIO. */
static void ram_wr64(unsigned int addr, unsigned int lo)
{
	*(volatile unsigned int *)addr = lo;
	*(volatile unsigned int *)(addr + 4) = 0;
}

static void zero_page(unsigned int addr, unsigned int len)
{
	unsigned int *p = (unsigned int *)addr;
	while (len >= 4) {
		*p++ = 0;
		len -= 4;
	}
}

static unsigned int op(unsigned int reg)
{
	return cap_len + reg;
}

static unsigned int rt(unsigned int reg)
{
	return rt_off + reg;
}

/* ---- rings ------------------------------------------------------------- */
static void ring_init(unsigned int base, unsigned int size,
		      unsigned int *enq, unsigned int *cycle)
{
	struct xhci_trb *link = (struct xhci_trb *)(base + (size - 1) * 16);

	link->p0 = base;
	link->p1 = 0;
	link->status = 0;
	link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
	*enq = 0;
	*cycle = 1;
}

static void ring_queue(unsigned int base, unsigned int size,
		       unsigned int *enq, unsigned int *cycle,
		       unsigned int p0, unsigned int p1,
		       unsigned int status, unsigned int control)
{
	struct xhci_trb *trb = (struct xhci_trb *)(base + *enq * 16);

	trb->p0 = p0;
	trb->p1 = p1;
	trb->status = status;
	__asm__ volatile ("" ::: "memory");
	trb->control = control | *cycle;
	__asm__ volatile ("" ::: "memory");

	(*enq)++;
	if (*enq == size - 1) {
		struct xhci_trb *link =
			(struct xhci_trb *)(base + (size - 1) * 16);
		__asm__ volatile ("" ::: "memory");
		link->control ^= TRB_CYCLE;
		*enq = 0;
		*cycle ^= 1;
	}
}

/* ---- event handling ---------------------------------------------------- */
static void xhci_poll_locked(void);
static void stop_int(unsigned int slot);
static void bulk_reset(struct xhci_bulk *b);

static void xhci_int_event(unsigned int slot, unsigned int ep,
			   unsigned int code, unsigned int residual)
{
	unsigned int i;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (!ints[i].active || ints[i].slot != slot ||
		    ints[i].ep_id != ep)
			continue;
		if (!ints[i].pending) {
			if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
				ints[i].pending_len =
					(ints[i].trb_len >= residual) ?
					ints[i].trb_len - residual : 0;
				ints[i].pending = 1;
			} else {
				ints[i].pending = 1;
				ints[i].error = 1;
			}
		}
		break;
	}
}

static void xhci_poll_locked(void)
{
	unsigned int n = 0;

	while (n++ < 512) {
		struct xhci_trb *ev =
			(struct xhci_trb *)(evt_ring + evt_idx * 16);
		unsigned int type;

		if ((ev->control & TRB_CYCLE) != evt_cycle)
			break;

		type = (ev->control >> 10) & 0x3F;

		if (type == TRB_COMPLETION) {
			unsigned int code = (ev->status >> 24) & 0xFF;
			unsigned int slot = (ev->control >> 24) & 0xFF;

			if (cmd_busy && ev->p0 == cmd_trb_addr) {
				cmd_status = code;
				cmd_slot = slot;
				cmd_done = 1;
			}
		} else if (type == TRB_TRANSFER) {
			unsigned int code = (ev->status >> 24) & 0xFF;
			unsigned int residual = ev->status & 0xFFFFFF;
			unsigned int slot = (ev->control >> 24) & 0xFF;
			unsigned int ep = (ev->control >> 16) & 0x1F;

			if (ctl_busy && slot == ctl_slot && ep == 1) {
				if (ev->p0 == ctl_data_trb) {
					ctl_actual = (ctl_req_len >= residual) ?
						ctl_req_len - residual : 0;
					ctl_data_seen = 1;
				} else if (ev->p0 == ctl_status_trb) {
					if (!ctl_data_seen)
						ctl_actual =
							(code == CC_SUCCESS) ?
							ctl_req_len : 0;
					ctl_status = code;
					ctl_done = 1;
				}
			} else if (out_busy && slot == out_slot &&
				   ep == bulk_out.ep_id &&
				   ev->p0 == out_trb) {
				out_status = code;
				out_done = 1;
			} else if (bulk_in.active && slot == bulk_in.slot &&
				   ep == bulk_in.ep_id) {
				bulk_in.armed = 0;
				if (!bulk_in.pending) {
					if (code == CC_SUCCESS ||
					    code == CC_SHORT_PACKET) {
						bulk_in.actual =
							(bulk_in.trb_len >=
							 residual) ?
							bulk_in.trb_len -
								residual : 0;
						bulk_in.pending = 1;
					} else {
						bulk_in.pending = 1;
						bulk_in.error = 1;
					}
				}
			} else {
				xhci_int_event(slot, ep, code, residual);
			}
		}

		evt_idx++;
		if (evt_idx >= XHCI_EVT_TRBS) {
			evt_idx = 0;
			evt_cycle ^= 1;
		}
	}

	mmio_wr64(rt(XHCI_RT_ERDP),
		  (evt_ring + evt_idx * 16) | 8);
}

/* ---- commands ---------------------------------------------------------- */
static int xhci_cmd(unsigned int p0, unsigned int p1, unsigned int control,
		    unsigned int *out_slot)
{
	unsigned int flags, t0, rc;

	if (!found)
		return -1;

	spin_lock_irqsave(&xhci_lock, &flags);
	if (cmd_busy) {
		spin_unlock_irqrestore(&xhci_lock, flags);
		return -1;
	}
	cmd_busy = 1;
	cmd_done = 0;
	cmd_status = 0;
	cmd_slot = 0;
	cmd_trb_addr = cmd_ring + cmd_enq * 16;
	ring_queue(cmd_ring, XHCI_CMD_TRBS, &cmd_enq, &cmd_cycle,
		   p0, p1, 0, control);
	__asm__ volatile ("" ::: "memory");
	mmio_wr(db_off, 0);
	spin_unlock_irqrestore(&xhci_lock, flags);

	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		spin_lock_irqsave(&xhci_lock, &flags);
		xhci_poll_locked();
		rc = cmd_done;
		spin_unlock_irqrestore(&xhci_lock, flags);
		if (rc)
			break;
		__asm__ volatile ("pause");
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	cmd_busy = 0;
	if (!cmd_done) {
		spin_unlock_irqrestore(&xhci_lock, flags);
		dbg("xhci: command timeout\n");
		return -1;
	}
	if (out_slot)
		*out_slot = cmd_slot;
	rc = cmd_status;
	spin_unlock_irqrestore(&xhci_lock, flags);
	if (rc != CC_SUCCESS) {
		dbg_hex("xhci: command status ", rc);
		return -1;
	}
	return 0;
}

/* ---- contexts ---------------------------------------------------------- */
static void fill_slot_ctx(unsigned int addr, unsigned int port,
			  unsigned int speed, unsigned int last_ctx)
{
	unsigned int *p = (unsigned int *)(input_ctx + ctx_size);

	p[0] = (speed << 20) | (last_ctx << 27);
	p[1] = (port + 1) << 16;
	p[2] = 0;
	p[3] = addr;
}

static void fill_ep0_ctx(unsigned int mps, unsigned int ring,
			 unsigned int enq, unsigned int cycle)
{
	unsigned int *p = (unsigned int *)(input_ctx + 2 * ctx_size);

	p[0] = 0;
	p[1] = (EP_CTRL << 3) | (3 << 1) | (mps << 16);
	p[2] = (ring + enq * 16) | cycle;
	p[3] = 0;
	p[4] = 8;
}

static void fill_ep_ctx(unsigned int ep_index, unsigned int type,
			unsigned int mps, unsigned int interval,
			unsigned int ring)
{
	unsigned int *p =
		(unsigned int *)(input_ctx + (ep_index + 2) * ctx_size);

	p[0] = interval << 16;
	p[1] = (type << 3) | (3 << 1) | (mps << 16);
	p[2] = ring | 1;
	p[3] = 0;
	p[4] = mps;
}

static struct xhci_slot *slot_by_addr(unsigned int addr)
{
	unsigned int id;

	if (addr > 127)
		return 0;
	id = addr_slot[addr];
	if (id < 1 || id > max_slots || !slots[id].used)
		return 0;
	return &slots[id];
}

static unsigned int port_speed(unsigned int port)
{
	unsigned int ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
	unsigned int sp = (ps >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;

	return sp ? sp : 1;
}

static int xhci_config_ep(struct xhci_slot *s, unsigned int ep_index,
			  unsigned int type, unsigned int mps,
			  unsigned int interval, unsigned int ring)
{
	unsigned int ep_flag = 1u << (ep_index + 1);
	unsigned int last = ep_index + 1;
	unsigned int i, flags;
	int rc;

	for (i = 1; i < 32; i++)
		if (s->ep_mask & (1u << i))
			last = i;

	spin_lock_irqsave(&xhci_lock, &flags);
	zero_page(input_ctx, 4096);
	*(unsigned int *)input_ctx = 0;
	*(unsigned int *)(input_ctx + 4) = SLOT_FLAG | ep_flag;
	fill_slot_ctx(s->addr, s->port, s->speed, last);
	fill_ep_ctx(ep_index, type, mps, interval, ring);
	spin_unlock_irqrestore(&xhci_lock, flags);

	rc = xhci_cmd(input_ctx, 0,
		      TRB_TYPE(TRB_CONFIG_EP) | (s->slot_id << 24), 0);
	if (rc == 0)
		s->ep_mask |= ep_flag;
	return rc;
}

/* ---- slots ------------------------------------------------------------- */
static struct xhci_slot *new_slot(void)
{
	struct xhci_slot *s;
	unsigned int slot_id, dc, ep0, flags;
	int rc;

	if (last_port < 0 || last_port >= (int)max_ports)
		return 0;
	if (port_slot[last_port])
		return 0;

	slot_id = 0;
	rc = xhci_cmd(0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot_id);
	if (rc != 0 || slot_id == 0 || slot_id > max_slots) {
		dbg("xhci: enable slot failed\n");
		return 0;
	}

	dc = mm_alloc_page();
	ep0 = mm_alloc_page();
	if (!dc || !ep0) {
		if (dc)
			mm_free_page(dc);
		if (ep0)
			mm_free_page(ep0);
		return 0;
	}
	zero_page(dc, 4096);
	zero_page(ep0, 4096);

	s = &slots[slot_id];
	s->used = 1;
	s->slot_id = slot_id;
	s->addr = 0;
	s->port = (unsigned int)last_port;
	s->speed = port_speed((unsigned int)last_port);
	s->dc = dc;
	s->ep0 = ep0;
	s->ep0_enq = 0;
	s->ep0_cycle = 1;
	s->ep_mask = 0;
	ring_init(ep0, XHCI_EP0_TRBS, &s->ep0_enq, &s->ep0_cycle);

	ram_wr64(dcbaa + slot_id * 8, dc);

	spin_lock_irqsave(&xhci_lock, &flags);
	zero_page(input_ctx, 4096);
	*(unsigned int *)input_ctx = 0;
	*(unsigned int *)(input_ctx + 4) = SLOT_FLAG | EP0_FLAG;
	fill_slot_ctx(0, s->port, s->speed, 1);
	fill_ep0_ctx(XHCI_CTRL_MPS, ep0, s->ep0_enq, s->ep0_cycle);
	spin_unlock_irqrestore(&xhci_lock, flags);

	rc = xhci_cmd(input_ctx, 0,
		      TRB_TYPE(TRB_ADDR_DEV) | (slot_id << 24) | TRB_BSR, 0);
	if (rc != 0) {
		dbg("xhci: address default failed\n");
		mm_free_page(dc);
		mm_free_page(ep0);
		s->used = 0;
		return 0;
	}

	addr_slot[0] = slot_id;
	port_slot[s->port] = slot_id;
	return s;
}

static void free_slot(struct xhci_slot *s)
{
	unsigned int i, flags;

	if (!s || !s->used)
		return;

	xhci_cmd(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (s->slot_id << 24), 0);
	ram_wr64(dcbaa + s->slot_id * 8, 0);

	spin_lock_irqsave(&xhci_lock, &flags);
	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (ints[i].slot == s->slot_id) {
			ints[i].active = 0;
			ints[i].pending = 0;
			ints[i].error = 0;
			if (ints[i].ring) {
				mm_free_page(ints[i].ring);
				ints[i].ring = 0;
			}
			ints[i].slot = 0;
		}
	}
	if (s->dc) {
		mm_free_page(s->dc);
		s->dc = 0;
	}
	if (s->ep0) {
		mm_free_page(s->ep0);
		s->ep0 = 0;
	}
	if (s->addr < 128)
		addr_slot[s->addr] = 0;
	if (bulk_in.active && bulk_in.slot == s->slot_id)
		bulk_reset(&bulk_in);
	if (bulk_out.active && bulk_out.slot == s->slot_id)
		bulk_reset(&bulk_out);
	if (s->port < XHCI_MAX_PORTS)
		port_slot[s->port] = 0;
	s->used = 0;
	s->addr = 0;
	s->ep_mask = 0;
	spin_unlock_irqrestore(&xhci_lock, flags);
}

static int set_address(unsigned int new_addr, unsigned int mps)
{
	struct xhci_slot *s = slot_by_addr(0);
	unsigned int flags;
	int rc;

	if (!s || new_addr == 0 || new_addr > 127)
		return -1;

	spin_lock_irqsave(&xhci_lock, &flags);
	zero_page(input_ctx, 4096);
	*(unsigned int *)input_ctx = 0;
	*(unsigned int *)(input_ctx + 4) = SLOT_FLAG | EP0_FLAG;
	fill_slot_ctx(new_addr, s->port, s->speed, 1);
	fill_ep0_ctx(mps, s->ep0, s->ep0_enq, s->ep0_cycle);
	spin_unlock_irqrestore(&xhci_lock, flags);

	rc = xhci_cmd(input_ctx, 0,
		      TRB_TYPE(TRB_ADDR_DEV) | (s->slot_id << 24), 0);
	if (rc != 0) {
		dbg("xhci: set address failed\n");
		return -1;
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	addr_slot[0] = 0;
	s->addr = new_addr;
	addr_slot[new_addr] = s->slot_id;
	spin_unlock_irqrestore(&xhci_lock, flags);
	return 0;
}

/* ---- control transfers ------------------------------------------------- */
static int control_transfer(unsigned int ctrl, unsigned int addr,
			    unsigned int mps, int low_speed,
			    const unsigned char *setup, unsigned char *data,
			    unsigned int len, int dir_in)
{
	struct xhci_slot *s;
	unsigned int flags, t0, rc, st;
	unsigned int setup_c, data_c, status_c;

	(void)ctrl;
	(void)low_speed;

	if (!found)
		return -1;

	/* xHCI owns SET_ADDRESS: it is an Address Device command, not a
	 * control request on the wire. */
	if (setup[0] == 0x00 && setup[1] == 0x05 && addr == 0)
		return set_address((unsigned int)setup[2] |
				   ((unsigned int)setup[3] << 8), mps);

	s = slot_by_addr(addr);
	if (!s) {
		if (addr != 0)
			return -1;
		s = new_slot();
		if (!s)
			return -1;
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	if (ctl_busy) {
		spin_unlock_irqrestore(&xhci_lock, flags);
		dbg("xhci: control busy\n");
		return -1;
	}
	ctl_busy = 1;
	ctl_done = 0;
	ctl_status = 0;
	ctl_actual = 0;
	ctl_data_seen = 0;
	ctl_slot = s->slot_id;
	ctl_req_len = len;

	setup_c = TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_CHAIN;
	if (len)
		setup_c |= (dir_in ? 3u : 2u) << 16;
	ctl_setup_trb = s->ep0 + s->ep0_enq * 16;
	ring_queue(s->ep0, XHCI_EP0_TRBS, &s->ep0_enq, &s->ep0_cycle,
		   (unsigned int)setup[0] | ((unsigned int)setup[1] << 8) |
		   ((unsigned int)setup[2] << 16) |
		   ((unsigned int)setup[3] << 24),
		   (unsigned int)setup[4] | ((unsigned int)setup[5] << 8) |
		   ((unsigned int)setup[6] << 16) |
		   ((unsigned int)setup[7] << 24),
		   8, setup_c);

	ctl_data_trb = 0;
	if (len) {
		ctl_data_trb = s->ep0 + s->ep0_enq * 16;
		data_c = TRB_TYPE(TRB_DATA) | TRB_CHAIN;
		if (dir_in)
			data_c |= TRB_DIR_IN | TRB_ISP;
		ring_queue(s->ep0, XHCI_EP0_TRBS, &s->ep0_enq,
			   &s->ep0_cycle, (unsigned int)data, 0, len, data_c);
	}

	ctl_status_trb = s->ep0 + s->ep0_enq * 16;
	status_c = TRB_TYPE(TRB_STATUS) | TRB_IOC;
	if (!len || !dir_in)
		status_c |= TRB_DIR_IN;
	ring_queue(s->ep0, XHCI_EP0_TRBS, &s->ep0_enq, &s->ep0_cycle,
		   0, 0, 0, status_c);

	__asm__ volatile ("" ::: "memory");
	mmio_wr(db_off + 4 * s->slot_id, 1);
	spin_unlock_irqrestore(&xhci_lock, flags);

	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		spin_lock_irqsave(&xhci_lock, &flags);
		xhci_poll_locked();
		rc = ctl_done;
		spin_unlock_irqrestore(&xhci_lock, flags);
		if (rc)
			break;
		__asm__ volatile ("pause");
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	if (!ctl_done) {
		ctl_busy = 0;
		spin_unlock_irqrestore(&xhci_lock, flags);
		dbg("xhci: control timeout\n");
		return -1;
	}
	rc = ctl_actual;
	st = ctl_status;
	ctl_busy = 0;
	spin_unlock_irqrestore(&xhci_lock, flags);

	if (st != CC_SUCCESS && st != CC_SHORT_PACKET) {
		dbg_hex("xhci: control status ", st);
		return -1;
	}
	return (int)rc;
}

/* ---- HID interrupt endpoint -------------------------------------------- */
static int int_in_start(unsigned int ctrl, unsigned int slot,
			unsigned int addr, unsigned int ep,
			unsigned int mps, int low_speed)
{
	struct xhci_slot *s = slot_by_addr(addr);
	struct xhci_int *in;
	unsigned int ep_index, ep_id, ring, flags;
	int rc;

	(void)ctrl;
	(void)low_speed;

	if (!found || !s || slot >= USB_HCD_INT_SLOTS || ep < 1 || ep > 15)
		return -1;
	if (mps < 1)
		mps = 8;
	if (mps > 8)
		mps = 8;

	if (ints[slot].active)
		stop_int(slot);

	ep_index = (ep - 1) * 2 + 2;	/* EP index 2 for EP1 IN, 4 for EP2 IN */
	ep_id = 2 * ep + 1;
	ring = mm_alloc_page();
	if (!ring)
		return -1;
	zero_page(ring, 4096);

	in = &ints[slot];
	in->slot = s->slot_id;
	in->ep_id = ep_id;
	in->mps = mps;
	in->ring = ring;
	in->enq = 0;
	in->cycle = 1;
	in->trb_len = mps;
	in->pending = 0;
	in->error = 0;
	in->active = 0;
	ring_init(ring, XHCI_INT_TRBS, &in->enq, &in->cycle);

	rc = xhci_config_ep(s, ep_index, EP_INT_IN, mps, 1, ring);
	if (rc != 0) {
		dbg_hex("xhci: configure int ep failed ", ep_index);
		mm_free_page(ring);
		in->ring = 0;
		return -1;
	}

	in->active = 1;
	spin_lock_irqsave(&xhci_lock, &flags);
	ring_queue(ring, XHCI_INT_TRBS, &in->enq, &in->cycle,
		   (unsigned int)in->buf, 0, in->trb_len,
		   TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	__asm__ volatile ("" ::: "memory");
	mmio_wr(db_off + 4 * s->slot_id, ep_id);
	spin_unlock_irqrestore(&xhci_lock, flags);

	return 0;
}

static int int_in_poll(unsigned int slot, unsigned char *buf, unsigned int len)
{
	struct xhci_int *in;
	unsigned int flags, n, i;

	if (!found || slot >= USB_HCD_INT_SLOTS)
		return 0;

	in = &ints[slot];
	if (!in->active)
		return 0;

	spin_lock_irqsave(&xhci_lock, &flags);
	xhci_poll_locked();
	spin_unlock_irqrestore(&xhci_lock, flags);

	if (!in->pending)
		return 0;
	if (in->error) {
		in->pending = 0;
		in->error = 0;
		return -1;
	}

	n = in->pending_len;
	if (n > len)
		n = len;
	for (i = 0; i < n; i++)
		buf[i] = in->buf[i];
	in->pending = 0;

	spin_lock_irqsave(&xhci_lock, &flags);
	if (in->active) {
		ring_queue(in->ring, XHCI_INT_TRBS, &in->enq, &in->cycle,
			   (unsigned int)in->buf, 0, in->trb_len,
			   TRB_TYPE(TRB_NORMAL) | TRB_IOC);
		__asm__ volatile ("" ::: "memory");
		mmio_wr(db_off + 4 * in->slot, in->ep_id);
	}
	spin_unlock_irqrestore(&xhci_lock, flags);

	return (int)in->pending_len;
}

static void stop_int(unsigned int slot)
{
	struct xhci_int *in;
	unsigned int flags;

	if (slot >= USB_HCD_INT_SLOTS)
		return;
	in = &ints[slot];
	spin_lock_irqsave(&xhci_lock, &flags);
	in->active = 0;
	in->pending = 0;
	in->error = 0;
	spin_unlock_irqrestore(&xhci_lock, flags);
}

/* ---- bulk transfers ---------------------------------------------------- */
static void bulk_reset(struct xhci_bulk *b)
{
	if (b->ring) {
		mm_free_page(b->ring);
		b->ring = 0;
	}
	b->active = 0;
	b->armed = 0;
	b->slot = 0;
	b->pending = 0;
	b->error = 0;
	b->actual = 0;
}

static int xhci_bulk_in(unsigned int ctrl, unsigned int addr, unsigned int ep,
		   unsigned int mps, unsigned char *buf, unsigned int len)
{
	struct xhci_slot *s = slot_by_addr(addr);
	struct xhci_bulk *b = &bulk_in;
	unsigned int ep_index, ep_id, ring, flags, n, err;
	int rc;

	(void)ctrl;
	if (!found || !s || ep < 1 || ep > 15 || !buf || !len)
		return -1;
	if (mps < 1)
		mps = 512;

	ep_index = (ep - 1) * 2 + 2;
	ep_id = 2 * ep + 1;

	if (!b->active || b->slot != s->slot_id || b->ep != ep) {
		ring = mm_alloc_page();
		if (!ring)
			return -1;
		zero_page(ring, 4096);
		bulk_reset(b);
		b->slot = s->slot_id;
		b->ep = ep;
		b->ep_id = ep_id;
		b->mps = mps;
		b->ring = ring;
		b->enq = 0;
		b->cycle = 1;
		ring_init(ring, XHCI_BULK_TRBS, &b->enq, &b->cycle);
		rc = xhci_config_ep(s, ep_index, EP_BULK_IN, mps, 0, ring);
		if (rc != 0) {
			bulk_reset(b);
			return -1;
		}
		b->active = 1;
	}

	b->buf = (unsigned int)buf;
	b->len = len;

	spin_lock_irqsave(&xhci_lock, &flags);
	xhci_poll_locked();
	if (b->pending) {
		n = b->actual;
		err = (unsigned int)b->error;
		b->pending = 0;
		b->error = 0;
		if (!err && b->active) {
			b->trb_len = len;
			ring_queue(b->ring, XHCI_BULK_TRBS, &b->enq,
				   &b->cycle, (unsigned int)buf, 0, len,
				   TRB_TYPE(TRB_NORMAL) | TRB_IOC);
			b->armed = 1;
			__asm__ volatile ("" ::: "memory");
			mmio_wr(db_off + 4 * b->slot, b->ep_id);
		}
		spin_unlock_irqrestore(&xhci_lock, flags);
		return err ? -1 : (int)n;
	}
	if (!b->armed) {
		b->trb_len = len;
		ring_queue(b->ring, XHCI_BULK_TRBS, &b->enq, &b->cycle,
			   (unsigned int)buf, 0, len,
			   TRB_TYPE(TRB_NORMAL) | TRB_IOC);
		b->armed = 1;
		__asm__ volatile ("" ::: "memory");
		mmio_wr(db_off + 4 * b->slot, b->ep_id);
	}
	spin_unlock_irqrestore(&xhci_lock, flags);
	return 0;
}

static int xhci_bulk_out(unsigned int ctrl, unsigned int addr, unsigned int ep,
		    unsigned int mps, const unsigned char *buf,
		    unsigned int len)
{
	struct xhci_slot *s = slot_by_addr(addr);
	struct xhci_bulk *b = &bulk_out;
	unsigned int ep_index, ep_id, ring, flags, t0, done, status;
	int rc;

	(void)ctrl;
	if (!found || !s || ep < 1 || ep > 15 || !buf || !len)
		return -1;
	if (mps < 1)
		mps = 512;

	ep_index = (ep - 1) * 2 + 1;
	ep_id = 2 * ep;

	if (!b->active || b->slot != s->slot_id || b->ep != ep) {
		ring = mm_alloc_page();
		if (!ring)
			return -1;
		zero_page(ring, 4096);
		bulk_reset(b);
		b->slot = s->slot_id;
		b->ep = ep;
		b->ep_id = ep_id;
		b->mps = mps;
		b->ring = ring;
		b->enq = 0;
		b->cycle = 1;
		ring_init(ring, XHCI_BULK_TRBS, &b->enq, &b->cycle);
		rc = xhci_config_ep(s, ep_index, EP_BULK_OUT, mps, 0, ring);
		if (rc != 0) {
			bulk_reset(b);
			return -1;
		}
		b->active = 1;
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	if (out_busy) {
		spin_unlock_irqrestore(&xhci_lock, flags);
		return -1;
	}
	out_busy = 1;
	out_done = 0;
	out_status = 0;
	out_slot = s->slot_id;
	out_trb = b->ring + b->enq * 16;
	b->trb_len = len;
	ring_queue(b->ring, XHCI_BULK_TRBS, &b->enq, &b->cycle,
		   (unsigned int)buf, 0, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
	__asm__ volatile ("" ::: "memory");
	mmio_wr(db_off + 4 * s->slot_id, ep_id);
	spin_unlock_irqrestore(&xhci_lock, flags);

	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		spin_lock_irqsave(&xhci_lock, &flags);
		xhci_poll_locked();
		done = out_done;
		spin_unlock_irqrestore(&xhci_lock, flags);
		if (done)
			break;
		__asm__ volatile ("pause");
	}

	spin_lock_irqsave(&xhci_lock, &flags);
	done = out_done;
	status = out_status;
	out_busy = 0;
	spin_unlock_irqrestore(&xhci_lock, flags);

	if (!done) {
		dbg("xhci: bulk out timeout\n");
		return -1;
	}
	if (status != CC_SUCCESS) {
		dbg_hex("xhci: bulk out status ", status);
		return -1;
	}
	return 0;
}

/* ---- root hub ports ---------------------------------------------------- */
static int port_reset(unsigned int ctrl, unsigned int port)
{
	unsigned int ps, t0;

	(void)ctrl;
	if (!found || port >= max_ports ||
	    !(usb2_mask & (1u << port)))
		return -1;

	if (port_slot[port])
		free_slot(&slots[port_slot[port]]);

	ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
	if (!(ps & PORTSC_CCS))
		return -1;

	mmio_wr(op(XHCI_OP_PORTSC) + port * 0x10,
		(ps & ~PORTSC_RWC) | PORTSC_PR);

	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 200) {
		ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
		if (ps & PORTSC_PRC)
			break;
	}
	if (!(ps & PORTSC_PRC)) {
		dbg_hex("xhci: port reset timeout ", port);
		return -1;
	}
	mmio_wr(op(XHCI_OP_PORTSC) + port * 0x10,
		(ps & ~PORTSC_RWC) | PORTSC_PRC);

	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 100) {
		ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
		if (ps & PORTSC_PED)
			break;
	}
	if (!(ps & PORTSC_PED)) {
		dbg_hex("xhci: port not enabled ", port);
		return -1;
	}

	last_port = (int)port;
	return 0;
}

static int port_connected(unsigned int ctrl, unsigned int port)
{
	unsigned int ps;

	(void)ctrl;
	if (!found || port >= max_ports ||
	    !(usb2_mask & (1u << port)))
		return 0;
	ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
	return (ps & PORTSC_CCS) ? 1 : 0;
}

static int port_low_speed(unsigned int ctrl, unsigned int port)
{
	unsigned int ps;

	(void)ctrl;
	if (!found || port >= max_ports)
		return 0;
	ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
	return ((ps >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK) == 2;
}

static int port_take_change(unsigned int ctrl, unsigned int port)
{
	unsigned int ps;

	(void)ctrl;
	if (!found || port >= max_ports)
		return 0;
	ps = mmio_rd(op(XHCI_OP_PORTSC) + port * 0x10);
	if (!(ps & PORTSC_CSC))
		return 0;
	mmio_wr(op(XHCI_OP_PORTSC) + port * 0x10,
		(ps & ~PORTSC_RWC) | PORTSC_CSC);
	return 1;
}

/* ---- controller discovery ---------------------------------------------- */
/* xHCI USB Legacy Support: stop the firmware's SMI from firing on port
 * changes once we own the controller (the same idea as the PCI 0xC0 write
 * in uhci.c/ohci.c, but xHCI keeps it in an extended capability). */
static void legacy_smi_disable(unsigned int off)
{
	unsigned int sup = mmio_rd(off);
	unsigned int ctl;

	if ((sup & 0xFF) != 1)
		return;
	if (sup & (1u << 16))		/* BIOS still owns it */
		mmio_wr(off, sup | (1u << 24));
	ctl = mmio_rd(off + 4);
	ctl &= ~((0x7u << 1) | (0xFFu << 5) | (0x7u << 17));
	ctl |= 0x7u << 29;
	mmio_wr(off + 4, ctl);
}

static void scan_protocols(void)
{
	unsigned int off, guard = 0;

	usb2_mask = 0;
	off = ((hcc_params >> 16) & 0xFFFF) << 2;

	while (off && guard++ < 32) {
		unsigned int cap = mmio_rd(off);
		unsigned int id = cap & 0xFF;
		unsigned int next = (cap >> 8) & 0xFF;

		if (id == 1) {
			legacy_smi_disable(off);
		} else if (id == 2) {	/* Supported Protocol */
			unsigned int major = (cap >> 24) & 0xFF;
			unsigned int pi = mmio_rd(off + 8);
			unsigned int po = pi & 0xFF;
			unsigned int pc = (pi >> 8) & 0xFF;
			unsigned int i;

			if (major <= 2 && po >= 1) {
				for (i = 0; i < pc && po + i <= 32; i++)
					usb2_mask |= 1u << (po - 1 + i);
			}
		}
		if (!next)
			break;
		off += next << 2;
	}

	if (!usb2_mask)
		usb2_mask = (max_ports >= 32) ? 0xFFFFFFFFu :
			    ((1u << max_ports) - 1);
}

static int xhci_init(void)
{
	unsigned int bus, dev, func, id, class_rev, bar, bar_hi, cmd;
	unsigned int hcs1, hcs2, t0, i, n, scratch;

	if (found)
		return 1;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			for (func = 0; func < 8; func++) {
				id = pci_cfg_read32(bus, dev, func, 0x00);
				if (id == 0xFFFFFFFFu || id == 0)
					continue;
				class_rev = pci_cfg_read32(bus, dev, func, 0x08);
				if ((class_rev >> 16) != 0x0C03)
					continue;
				if (((class_rev >> 8) & 0xFF) != 0x30)
					continue;
				bar = pci_cfg_read32(bus, dev, func, 0x10);
				if (bar & 1)
					continue;
				bar_hi = 0;
				if ((bar & 0x6) == 0x4)
					bar_hi = pci_cfg_read32(bus, dev,
								func, 0x14);
				bar &= 0xFFFFFFF0u;
				if (!bar || bar_hi)
					continue;
				pci_bus = bus;
				pci_dev = dev;
				pci_func = func;
				goto found_xhci;
			}
		}
	}
	return 0;

found_xhci:
	cmd = pci_cfg_read32(pci_bus, pci_dev, pci_func, 0x04);
	pci_cfg_write32(pci_bus, pci_dev, pci_func, 0x04, cmd | 0x0006);

	mmio_base = bar;
	paging_map_mmio(mmio_base, 0x10000);

	cap_len = mmio_rd(0) & 0xFF;
	hcs1 = mmio_rd(XHCI_CAP_HCSPARAMS1);
	hcs2 = mmio_rd(XHCI_CAP_HCSPARAMS2);
	hcc_params = mmio_rd(XHCI_CAP_HCCPARAMS1);
	rt_off = mmio_rd(XHCI_CAP_RTSOFF) & ~0x1Fu;
	db_off = mmio_rd(XHCI_CAP_DBOFF) & ~0x3u;

	max_slots = hcs1 & 0xFF;
	if (max_slots > XHCI_MAX_SLOTS)
		max_slots = XHCI_MAX_SLOTS;
	if (!max_slots)
		max_slots = 1;
	max_ports = (hcs1 >> 24) & 0xFF;
	if (max_ports > XHCI_MAX_PORTS)
		max_ports = XHCI_MAX_PORTS;
	if (!max_ports)
		max_ports = 1;
	ctx_size = (hcc_params & 0x0004u) ? 64 : 32;

	dbg("xhci: found\n");
	dbg_hex("xhci: mmio ", mmio_base);
	dbg_dec("xhci: slots ", max_slots);
	dbg_dec("xhci: ports ", max_ports);
	dbg_dec("xhci: cap len ", cap_len);

	dcbaa = mm_alloc_page();
	input_ctx = mm_alloc_page();
	cmd_ring = mm_alloc_page();
	evt_ring = mm_alloc_page();
	erst = mm_alloc_page();
	if (!dcbaa || !input_ctx || !cmd_ring || !evt_ring || !erst) {
		dbg("xhci: out of pages\n");
		return 0;
	}
	zero_page(dcbaa, 4096);
	zero_page(input_ctx, 4096);
	zero_page(cmd_ring, 4096);
	zero_page(evt_ring, 4096);
	zero_page(erst, 4096);

	/* Reset.  HCRST self-clears; wait for CNR to drop. */
	mmio_wr(op(XHCI_OP_USBCMD), 0);
	mmio_wr(op(XHCI_OP_USBCMD), XHCI_CMD_HCRST);
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		if (!(mmio_rd(op(XHCI_OP_USBCMD)) & XHCI_CMD_HCRST))
			break;
	}
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		if (!(mmio_rd(op(XHCI_OP_USBSTS)) & XHCI_STS_CNR))
			break;
	}
	if (mmio_rd(op(XHCI_OP_USBSTS)) & (XHCI_STS_CNR | XHCI_STS_HCE)) {
		dbg_hex("xhci: reset failed ",
			mmio_rd(op(XHCI_OP_USBSTS)));
		return 0;
	}

	mmio_wr(op(XHCI_OP_PAGESIZE), 1);
	mmio_wr(op(XHCI_OP_CONFIG), max_slots);
	mmio_wr64(op(XHCI_OP_DCBAAP), dcbaa);

	/* Scratchpad buffers, if the controller asks for any. */
	n = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
	if (n) {
		unsigned int arr;

		if (n > 512)
			n = 512;
		arr = mm_alloc_page();
		if (!arr) {
			dbg("xhci: no scratchpad array\n");
			return 0;
		}
		zero_page(arr, 4096);
		for (i = 0; i < n; i++) {
			scratch = mm_alloc_page();
			if (!scratch) {
				dbg("xhci: no scratchpad page\n");
				return 0;
			}
			zero_page(scratch, 4096);
			ram_wr64(arr + i * 8, scratch);
		}
		ram_wr64(dcbaa, arr);
		dbg_dec("xhci: scratchpads ", n);
	}

	/* Command ring and event ring. */
	ring_init(cmd_ring, XHCI_CMD_TRBS, &cmd_enq, &cmd_cycle);
	mmio_wr64(op(XHCI_OP_CRCR), cmd_ring | 1);

	evt_idx = 0;
	evt_cycle = 1;
	*(volatile unsigned int *)(erst + 0) = evt_ring;
	*(volatile unsigned int *)(erst + 4) = 0;
	*(volatile unsigned int *)(erst + 8) = XHCI_EVT_TRBS;
	*(volatile unsigned int *)(erst + 12) = 0;
	mmio_wr(rt(XHCI_RT_ERSTSZ), 1);
	mmio_wr64(rt(XHCI_RT_ERSTBA), erst);
	mmio_wr64(rt(XHCI_RT_ERDP), evt_ring);
	mmio_wr(rt(XHCI_RT_IMAN), 0);
	mmio_wr(rt(XHCI_RT_IMOD), 0);

	/* Run. */
	mmio_wr(op(XHCI_OP_USBCMD), XHCI_CMD_RS);
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 1000) {
		if (!(mmio_rd(op(XHCI_OP_USBSTS)) & XHCI_STS_HCH))
			break;
	}
	if (mmio_rd(op(XHCI_OP_USBSTS)) & XHCI_STS_HCH) {
		dbg("xhci: controller did not start\n");
		return 0;
	}

	scan_protocols();
	dbg_hex("xhci: usb2 ports ", usb2_mask);

	/* Port power, where the controller has per-port switching. */
	if (hcc_params & 0x0008u) {
		for (i = 0; i < max_ports; i++) {
			unsigned int ps;
			if (!(usb2_mask & (1u << i)))
				continue;
			ps = mmio_rd(op(XHCI_OP_PORTSC) + i * 0x10);
			if (!(ps & PORTSC_PP))
				mmio_wr(op(XHCI_OP_PORTSC) + i * 0x10,
					(ps & ~PORTSC_RWC) | PORTSC_PP);
		}
	}

	found = 1;
	return 1;
}

static unsigned int xhci_count(void)
{
	return found ? 1u : 0u;
}

static void xhci_info(unsigned int ctrl, unsigned int *bus,
		      unsigned int *dev, unsigned int *func,
		      unsigned int *base, int *mmio)
{
	(void)ctrl;
	*bus = pci_bus;
	*dev = pci_dev;
	*func = pci_func;
	*base = mmio_base;
	*mmio = 1;
}

static unsigned int xhci_port_count(unsigned int ctrl)
{
	(void)ctrl;
	return found ? max_ports : 0;
}

const struct usb_hcd_ops usb_xhci_ops = {
	"xHCI",
	xhci_init,
	xhci_count,
	xhci_info,
	control_transfer,
	xhci_port_count,
	port_connected,
	port_low_speed,
	port_reset,
	port_take_change,
	int_in_start,
	int_in_poll,
	stop_int,
	xhci_bulk_out,
	xhci_bulk_in,
};
