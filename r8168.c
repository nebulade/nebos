/* Realtek RTL8111/8168 Gigabit Ethernet driver (C+ mode, 8168c/d/e class).
 *
 * Target chip: the RTL8112L on the ASUS AT3IONT-I -- PCI 10ec:8168 rev 03,
 * register window in BAR2 (64-bit, 4 KB, prefetchable).  Polled: no IRQ.
 * TX/RX use 16-byte descriptors the NIC DMAs to/from memory; register and
 * descriptor values mirror the r8169 sequence from Linux.
 *
 * Every wait is time-bounded and each bring-up step writes a kdbg info row
 * (and serial), because the target machine has no serial console: a wedged
 * driver must still be diagnosable from the screen (progress row + PIT
 * heartbeat).
 */

#include "io.h"
#include "kdbg.h"
#include "mm.h"
#include "netdev.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "serial.h"
#include "timer.h"
#include "r8168.h"

/* ---- chip registers (offsets into the MMIO window) ---- */
#define REG_MAC0         0x00
#define REG_TxDescLow    0x20
#define REG_TxDescHigh   0x24
#define REG_ChipCmd      0x37
#define REG_TxPoll       0x38
#define REG_IntrMask     0x3c
#define REG_IntrStatus   0x3e
#define REG_TxConfig     0x40   /* also carries the chip version (XID) */
#define REG_RxConfig     0x44
#define REG_Cfg9346      0x50
#define REG_PHYAR        0x60
#define REG_PHYstatus    0x6c
#define REG_RDSAR1       0xd0   /* RX ring high 32 bits (8168c only) */
#define REG_RxMaxSize    0xda
#define REG_CPlusCmd     0xe0
#define REG_IntrMitigate 0xe2
#define REG_RxDescLow    0xe4
#define REG_RxDescHigh   0xe8
#define REG_MaxTxPacket  0xec

/* ChipCmd bits. */
#define ChipCmd_Reset    0x10
#define ChipCmd_RxEnb    0x08
#define ChipCmd_TxEnb    0x04

#define Cfg9346_Unlock   0xC0

/* TxPoll: kick the normal priority queue. */
#define TxPoll_NPQ       0x40

/* TxConfig: max PCI burst + shortest inter-frame gap. */
#define TXCONFIG_VALUE   ((7 << 8) | (3 << 24))

/* RxConfig. */
#define RX_DMA_BURST     0x0700
#define RX128_INT_EN     0x8000
#define RX_ACCEPT_MASK   0x3f
#define RX_ACCEPT_BROADCAST 0x08
#define RX_ACCEPT_MULTICAST 0x04
#define RX_ACCEPT_MYPHYS    0x02
#define RX_ACCEPT_ALLPHYS   0x01   /* promiscuous (bring-up: see set_rx_mode) */

/* PHYstatus (0x6c). */
#define PHYST_LINK       0x02
#define PHYST_FULLDUP    0x01
#define PHYST_1000       0x10
#define PHYST_100        0x08
#define PHYST_10         0x04

/* PHYAR: bit 31 = busy/valid, reg in bits 20:16, data in 15:0. */
#define PHYAR_FLAG       0x80000000u

/* Interrupt/event status bits (IntrStatus, 0x3e).  The register latches
 * events; each bit is cleared by writing it back.  With IntrMask = 0 we only
 * use these as a diagnostic and to detect an RX stall. */
#define ISR_RxOK         0x0001
#define ISR_RxErr        0x0002
#define ISR_TxOK         0x0004
#define ISR_TxErr        0x0008
#define ISR_ROVW         0x0010   /* RX FIFO overflow */
#define ISR_LinkChg      0x0020
#define ISR_FOV          0x0040   /* RX FIFO overflow (8168) */
#define ISR_TDU          0x0080   /* TX descriptor unavailable */
#define ISR_SWInt        0x0100

/* PHY registers. */
#define MII_BMCR         0
#define MII_BMSR         1
#define MII_PHYSID1      2
#define MII_PHYSID2      3
#define MII_ANAR         4
#define MII_GBSR         9

#define BMCR_AUTONEG     0x1000
#define BMCR_RESTART_AN  0x0200
#define BMSR_LINK        0x0004

/* Descriptor bits (C+ mode). */
#define DescOwn          0x80000000u
#define DescRingEnd      0x40000000u
#define DescFirstFrag    0x20000000u
#define DescLastFrag     0x10000000u
#define RxRES            (1u << 21)   /* RX error summary */
#define RxPktLenMask     0x3fffu      /* length field, includes the FCS */

/* Rings. */
#define R8168_NTX        16
#define R8168_NRX        16
#define R8168_BUF_SIZE   2048
#define R8168_RX_ARM     (R8168_BUF_SIZE - 1)

/* PCI ids. */
#define R8168_VENDOR     0x10ECu
#define R8168_DEVICE     0x8168u

struct r8168_desc {
	volatile unsigned int opts1;
	volatile unsigned int opts2;
	volatile unsigned int addr_lo;
	volatile unsigned int addr_hi;
};

static unsigned char *mmio;
static int present;
static int inited;
static unsigned char mac[6];
static unsigned int xid;               /* (TxConfig >> 20) & 0xfcf */
static unsigned int bar_base, bar_size;
static unsigned int pci_bus, pci_dev, pci_func;

static struct r8168_desc *tx_ring;
static struct r8168_desc *rx_ring;
static unsigned char *tx_bufs;
static unsigned char *rx_bufs;
static unsigned int tx_cur;
static unsigned int rx_cur;

/* Counters (mirrored into the kdbg status row). */
static unsigned int tx_ok, tx_err, rx_ok, rx_err, rx_drop;
static unsigned int last_tx_status;     /* raw descriptor status after TX */
static unsigned int isr_last;           /* last IntrStatus value we acked */
static unsigned int isr_rovw, isr_fov, isr_tdu;
static unsigned int rx_restarts;

static int kdbg_stat_row = -1;          /* link + counters */

static int timed_out(unsigned int start_ms, unsigned int ms)
{
	return get_ticks_ms() - start_ms > ms;
}

/* ---- MMIO accessors (uncacheable mapping) ---- */

static inline unsigned int r32(unsigned int reg)
{
	return *(volatile unsigned int *)(mmio + reg);
}

static inline void w32(unsigned int reg, unsigned int v)
{
	*(volatile unsigned int *)(mmio + reg) = v;
}

static inline unsigned short r16(unsigned int reg)
{
	return *(volatile unsigned short *)(mmio + reg);
}

static inline void w16(unsigned int reg, unsigned short v)
{
	*(volatile unsigned short *)(mmio + reg) = v;
}

static inline unsigned char r8(unsigned int reg)
{
	return *(volatile unsigned char *)(mmio + reg);
}

static inline void w8(unsigned int reg, unsigned char v)
{
	*(volatile unsigned char *)(mmio + reg) = v;
}

/* Keep the compiler from reordering descriptor/MMIO stores.  x86 caches are
 * coherent with PCI DMA, so no instruction barrier is needed. */
static inline void dma_barrier(void)
{
	__asm__ volatile ("" ::: "memory");
}

/* ---- small string/decimal/hex buffer helpers ---- */

static unsigned int app(char *b, unsigned int i, const char *s)
{
	while (*s)
		b[i++] = *s++;
	return i;
}

static unsigned int appd(char *b, unsigned int i, unsigned int v)
{
	char t[11];
	unsigned int n = 0;

	if (v > 9999)
		v = 9999;               /* keep rows short */
	if (v == 0) {
		b[i++] = '0';
		return i;
	}
	while (v) {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (n)
		b[i++] = t[--n];
	return i;
}

static unsigned int apph(char *b, unsigned int i, unsigned int v, unsigned int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned int d;

	for (d = 0; d < digits; d++)
		b[i++] = hex[(v >> (4 * (digits - 1 - d))) & 0xF];
	return i;
}

/* ---- console printing helpers ---- */

static void put_hex1(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[v & 0xF]);
}

static void put_hex2(unsigned int v)
{
	put_hex1((v >> 4) & 0xF);
	put_hex1(v & 0xF);
}

static void put_hex4(unsigned int v)
{
	put_hex2((v >> 8) & 0xFF);
	put_hex2(v & 0xFF);
}

static void put_dec(unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		io_putchar('0');
		return;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		io_putchar(b[--i]);
}

/* ---- debug trail (kdbg rows + serial) ---- */

/* Each step advances to a fresh info row (up to R8168_TRAIL_ROWS) so a photo
 * of the screen shows the bring-up sequence (PCI -> reset -> ... -> link);
 * after that the last row is reused, which keeps the most recent step
 * visible.  The row count is capped because the pool is shared with the MTRR
 * dump (11 rows on the AT3IONT-I). */
#define R8168_TRAIL_ROWS 2

static int trail_row = -1;
static int trail_used;

static void dbg(const char *s)
{
	if (trail_row < 0) {
		trail_row = kdbg_info_alloc();
		trail_used = 1;
	} else if (trail_used < R8168_TRAIL_ROWS) {
		int next = kdbg_info_alloc();

		if (next >= 0) {
			trail_row = next;
			trail_used++;
		}
	}
	if (trail_row >= 0)
		kdbg_info((unsigned int)trail_row, s);
	serial_puts(SERIAL_COM1, "r8168: ");
	serial_puts(SERIAL_COM1, s);
	serial_puts(SERIAL_COM1, "\n");
}

static const char *chip_name(void)
{
	switch (xid & 0x7c8) {
	case 0x280: return "RTL8168d/8111d";   /* RTL8112L class */
	case 0x3c0: return "RTL8168c/8111c";
	case 0x3c8: return "RTL8168cp/8111cp";
	case 0x380: return "RTL8168b/8111b";
	case 0x2c0: return "RTL8168e/8111e";
	default:    return "RTL8168/8111 (unknown rev)";
	}
}

/* ---- PHY access via PHYAR ---- */

static int phy_busy(void)
{
	unsigned int t0 = get_ticks_ms();

	while (r32(REG_PHYAR) & PHYAR_FLAG) {
		if (timed_out(t0, 100))
			return -1;
	}
	return 0;
}

static int mdio_write(unsigned int reg, unsigned int val)
{
	if (phy_busy() != 0)
		return -1;
	w32(REG_PHYAR, PHYAR_FLAG | ((reg & 0x1f) << 16) | (val & 0xffff));
	return 0;
}

static int mdio_read(unsigned int reg)
{
	unsigned int t0 = get_ticks_ms();

	if (phy_busy() != 0)
		return -1;
	w32(REG_PHYAR, (reg & 0x1f) << 16);
	while (!(r32(REG_PHYAR) & PHYAR_FLAG)) {
		if (timed_out(t0, 100))
			return -1;
	}
	return (int)(r32(REG_PHYAR) & 0xffff);
}

/* ---- status row ---- */

/* "1000M"/"100M"/"10M"/"?" + duplex letter, into out[6]. */
static void speed_string(unsigned int st, char *out)
{
	if (st & PHYST_1000) {
		out[0] = '1'; out[1] = '0'; out[2] = '0'; out[3] = '0'; out[4] = 'M';
	} else if (st & PHYST_100) {
		out[0] = '1'; out[1] = '0'; out[2] = '0'; out[3] = 'M'; out[4] = ' ';
	} else if (st & PHYST_10) {
		out[0] = '1'; out[1] = '0'; out[2] = 'M'; out[3] = ' '; out[4] = ' ';
	} else {
		out[0] = '?'; out[1] = out[2] = out[3] = out[4] = ' ';
	}
	out[5] = '\0';
}

/* "NIC U 1000MF TX 3/0 RX 5/0" into the kdbg status row. */
static void status_line(void)
{
	char buf[40];
	unsigned int st = r8(REG_PHYstatus);
	unsigned int i = 0;

	if (kdbg_stat_row < 0)
		kdbg_stat_row = kdbg_info_alloc();   /* reserve the row up front */

	if (kdbg_stat_row < 0)
		return;

	i = app(buf, i, "NIC ");
	buf[i++] = (st & PHYST_LINK) ? 'U' : 'D';
	buf[i++] = ' ';
	{
		char sp[6];
		unsigned int j;

		speed_string(st, sp);
		for (j = 0; sp[j]; j++)
			buf[i++] = sp[j];
	}
	buf[i++] = (st & PHYST_FULLDUP) ? 'F' : 'H';
	i = app(buf, i, " TX ");
	i = appd(buf, i, tx_ok);
	buf[i++] = '/';
	i = appd(buf, i, tx_err);
	i = app(buf, i, " RX ");
	i = appd(buf, i, rx_ok);
	buf[i++] = '/';
	i = appd(buf, i, rx_err);
	buf[i] = '\0';

	if (kdbg_stat_row < 0)
		kdbg_stat_row = kdbg_info_alloc();
	if (kdbg_stat_row >= 0)
		kdbg_info((unsigned int)kdbg_stat_row, buf);
}

/* Re-arm every RX descriptor and restart the RX engine.  The chip can halt
 * its RX DMA after a FIFO overflow or when it ran out of descriptors (which
 * happens when nothing polls for a while, e.g. while a command is printing);
 * re-asserting CmdRxEnb resumes it. */
static void rx_restart(void)
{
	unsigned int i;

	w8(REG_ChipCmd, ChipCmd_TxEnb);        /* stop RX */
	for (i = 0; i < 1000; i++)             /* let the engine settle */
		(void)r8(REG_ChipCmd);
	for (i = 0; i < R8168_NRX; i++) {
		rx_ring[i].opts2 = 0;
		rx_ring[i].addr_lo = (unsigned int)(rx_bufs + i * R8168_BUF_SIZE);
		rx_ring[i].addr_hi = 0;
		rx_ring[i].opts1 = DescOwn | R8168_RX_ARM |
				   (i == R8168_NRX - 1 ? DescRingEnd : 0);
	}
	dma_barrier();
	rx_cur = 0;
	w8(REG_ChipCmd, ChipCmd_RxEnb | ChipCmd_TxEnb);
	rx_restarts++;
}

/* Read + acknowledge latched events; recover from an RX stall. */
static void isr_poll(void)
{
	unsigned int st = r16(REG_IntrStatus);

	if (st == 0)
		return;
	w16(REG_IntrStatus, st);               /* write back to clear */
	isr_last = st;
	if (st & ISR_ROVW) {
		isr_rovw++;
		rx_restart();
	}
	if (st & ISR_FOV) {
		isr_fov++;
		rx_restart();
	}
	if (st & ISR_TDU) {
		isr_tdu++;
		w8(REG_TxPoll, TxPoll_NPQ);    /* nudge the TX queue */
	}
}

/* ---- PCI probe ---- */

static int bar_is_mmio(unsigned int val)
{
	return !(val & 1u) && (val & 0xFFFFFFF0u);
}

static int probe_pci(void)
{
	unsigned int bus, dev, func, bar;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int htype, nfunc;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			/* Multifunction: scan every function. */
			htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;

			for (func = 0; func < nfunc; func++) {
				unsigned int idf = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int cmd;

				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;
				if ((idf & 0xFFFFu) != R8168_VENDOR)
					continue;
				if (((idf >> 16) & 0xFFFFu) != R8168_DEVICE)
					continue;

				/* Enable memory space + bus master. */
				cmd = pci_cfg_read32(bus, dev, func, 0x04);
				pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0006u);

				/* First memory BAR (BAR2 on this chip). */
				for (bar = 0; bar < 6; bar++) {
					unsigned int val = pci_cfg_read32(bus, dev, func,
									  0x10 + 4 * bar);
					unsigned int hi = 0, mask;

					if (!bar_is_mmio(val))
						continue;

					if (val & 4u)   /* 64-bit BAR: high half */
						hi = pci_cfg_read32(bus, dev, func,
								    0x10 + 4 * bar + 4);

					/* Size probe: all-ones, read mask, restore. */
					pci_cfg_write32(bus, dev, func, 0x10 + 4 * bar,
							0xFFFFFFFFu);
					if (val & 4u)
						pci_cfg_write32(bus, dev, func,
								0x10 + 4 * bar + 4,
								0xFFFFFFFFu);
					mask = pci_cfg_read32(bus, dev, func,
							      0x10 + 4 * bar) & 0xFFFFFFF0u;
					pci_cfg_write32(bus, dev, func, 0x10 + 4 * bar, val);
					if (val & 4u)
						pci_cfg_write32(bus, dev, func,
								0x10 + 4 * bar + 4, hi);

					if (mask == 0)
						continue;

					bar_base = val & 0xFFFFFFF0u;
					bar_size = (~mask + 1u);
					if (bar_size < 0x1000)
						bar_size = 0x1000;
					pci_bus = bus;
					pci_dev = dev;
					pci_func = func;
					return 0;
				}
			}
		}
	}
	return -1;
}

/* ---- bring-up steps ---- */

static int hw_reset(void)
{
	unsigned int t0;

	w8(REG_ChipCmd, ChipCmd_Reset);
	t0 = get_ticks_ms();
	while (r8(REG_ChipCmd) & ChipCmd_Reset) {
		if (timed_out(t0, 100))
			return -1;
	}
	return 0;
}

static void read_mac(void)
{
	unsigned int v, i;

	w8(REG_Cfg9346, Cfg9346_Unlock);
	v = r32(REG_MAC0);
	for (i = 0; i < 4; i++)
		mac[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
	v = r16(REG_MAC0 + 4);
	mac[4] = (unsigned char)(v & 0xFF);
	mac[5] = (unsigned char)((v >> 8) & 0xFF);

	/* Program the address back into the chip.  The MAC filter registers
	 * (IDR/RAR) must hold our address or unicast frames are dropped, and a
	 * soft reset may have cleared them; every driver does this after a
	 * reset.  Order matters: MAC4 (high word) first, then MAC0. */
	w16(REG_MAC0 + 4, (unsigned short)(mac[4] | (mac[5] << 8)));
	w32(REG_MAC0, (unsigned int)mac[0] | ((unsigned int)mac[1] << 8) |
		      ((unsigned int)mac[2] << 16) | ((unsigned int)mac[3] << 24));
	w8(REG_Cfg9346, 0x00);
}

static void setup_rings(void)
{
	unsigned int i;

	w32(REG_TxDescHigh, 0);
	w32(REG_TxDescLow, (unsigned int)tx_ring);
	w32(REG_RxDescHigh, 0);
	w32(REG_RxDescLow, (unsigned int)rx_ring);
	if ((xid & 0x7c8) == 0x3c0 || (xid & 0x7c8) == 0x3c8)
		w32(REG_RDSAR1, 0);   /* 8168c takes the RX ring high half too */

	for (i = 0; i < R8168_NTX; i++) {
		tx_ring[i].opts1 = 0;
		tx_ring[i].opts2 = 0;
		tx_ring[i].addr_lo = (unsigned int)(tx_bufs + i * R8168_BUF_SIZE);
		tx_ring[i].addr_hi = 0;
	}
	for (i = 0; i < R8168_NRX; i++) {
		rx_ring[i].opts2 = 0;
		rx_ring[i].addr_lo = (unsigned int)(rx_bufs + i * R8168_BUF_SIZE);
		rx_ring[i].addr_hi = 0;
		rx_ring[i].opts1 = DescOwn | R8168_RX_ARM;
	}
	rx_ring[R8168_NRX - 1].opts1 |= DescRingEnd;
	dma_barrier();
	tx_cur = 0;
	rx_cur = 0;
}

static void set_rx_mode(void)
{
	unsigned int cfg = r32(REG_RxConfig);

	/* Promiscuous for bring-up (same choice as the ne2000 driver): keeps
	 * NIC and switch filtering out of the picture while we validate the
	 * DMA path; netrecv is a diagnosis tool and wants to see everything. */
	cfg = (cfg & ~RX_ACCEPT_MASK) |
	      RX_ACCEPT_BROADCAST | RX_ACCEPT_MULTICAST | RX_ACCEPT_MYPHYS |
	      RX_ACCEPT_ALLPHYS;
	w32(REG_RxConfig, cfg);
}

static void phy_autoneg(void)
{
	int id1 = mdio_read(MII_PHYSID1);
	int id2 = mdio_read(MII_PHYSID2);

	serial_puts(SERIAL_COM1, "r8168: phy id ");
	serial_puthex(SERIAL_COM1, (unsigned int)(id1 < 0 ? 0xFFFF : id1));
	serial_puts(SERIAL_COM1, ":");
	serial_puthex(SERIAL_COM1, (unsigned int)(id2 < 0 ? 0xFFFF : id2));
	serial_puts(SERIAL_COM1, "\n");

	mdio_write(MII_ANAR, 0x01E1);   /* advertise 10/100 half+full */
	mdio_write(MII_GBSR, 0x0300);   /* advertise 1000 half+full */
	mdio_write(MII_BMCR, BMCR_AUTONEG | BMCR_RESTART_AN);
}

static void wait_link(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 <= ms) {
		int bmsr = mdio_read(MII_BMSR);

		if ((bmsr >= 0 && (bmsr & BMSR_LINK)) ||
		    (r8(REG_PHYstatus) & PHYST_LINK))
			return;
	}
}

/* ---- netdev interface ---- */

/* Rings + configuration registers + PHY + RX/TX enable.  Used by both the
 * first probe and `netreset`. */
static void hw_start(void)
{
	w8(REG_Cfg9346, Cfg9346_Unlock);
	setup_rings();
	w32(REG_RxConfig, RX128_INT_EN | RX_DMA_BURST);
	set_rx_mode();
	w32(REG_TxConfig, TXCONFIG_VALUE);
	w16(REG_RxMaxSize, R8168_BUF_SIZE);
	w8(REG_MaxTxPacket, 0x3f);
	w16(REG_IntrMitigate, 0x0000);
	w16(REG_IntrMask, 0x0000);
	w16(REG_IntrStatus, 0xFFFF);
	w8(REG_Cfg9346, 0x00);
	dbg("regs ok");

	phy_autoneg();
	dbg("phy up");

	w8(REG_ChipCmd, ChipCmd_RxEnb | ChipCmd_TxEnb);

	/* Give autoneg a moment, then wait for link (bounded). */
	{
		unsigned int t0 = get_ticks_ms();

		while (get_ticks_ms() - t0 < 3000) {
			if (r8(REG_PHYstatus) & PHYST_LINK)
				break;
		}
	}
	wait_link(2000);
	dbg(r8(REG_PHYstatus) & PHYST_LINK ? "link up" : "no link");
}

int r8168_init(void)
{
	char buf[40];
	unsigned int i;

	if (inited)
		return present ? 0 : -1;
	inited = 1;

	/* Reserve the status row before the trail grabs rows. */
	if (kdbg_stat_row < 0)
		kdbg_stat_row = kdbg_info_alloc();

	if (probe_pci() != 0) {
		dbg("no 10ec:8168 found");
		return -1;
	}

	/* "PCI 04:00.0 f8fff000 4K" */
	i = 0;
	i = app(buf, i, "PCI ");
	i = apph(buf, i, pci_bus, 2);
	buf[i++] = ':';
	i = apph(buf, i, pci_dev, 2);
	buf[i++] = '.';
	buf[i++] = (char)('0' + pci_func);
	buf[i++] = ' ';
	i = apph(buf, i, bar_base, 8);
	buf[i++] = ' ';
	i = appd(buf, i, bar_size / 1024);
	buf[i++] = 'K';
	buf[i] = '\0';
	dbg(buf);

	process_map_mmio_all(bar_base, bar_size);
	mmio = (unsigned char *)bar_base;

	/* Master abort / unmapped window reads back all-ones. */
	if (r32(REG_TxConfig) == 0xFFFFFFFFu) {
		dbg("mmio read failed");
		mmio = 0;
		return -1;
	}

	xid = (r32(REG_TxConfig) >> 20) & 0xfcf;

	if (hw_reset() != 0) {
		dbg("reset timeout");
		mmio = 0;
		return -1;
	}
	dbg("reset ok");

	read_mac();

	/* Ring memory: one page each (256-byte aligned); buffers in pages. */
	tx_ring = (struct r8168_desc *)mm_alloc_page();
	rx_ring = (struct r8168_desc *)mm_alloc_page();
	tx_bufs = (unsigned char *)mm_alloc_pages(R8168_NTX * R8168_BUF_SIZE / PAGE_SIZE);
	rx_bufs = (unsigned char *)mm_alloc_pages(R8168_NRX * R8168_BUF_SIZE / PAGE_SIZE);
	if (!tx_ring || !rx_ring || !tx_bufs || !rx_bufs) {
		dbg("no memory for rings");
		mmio = 0;
		return -1;
	}
	dbg("ring memory ok");

	hw_start();

	/* "<chip> XID <xxx> <mac>" */
	i = 0;
	i = app(buf, i, chip_name());
	buf[i++] = ' ';
	i = app(buf, i, "XID ");
	i = apph(buf, i, xid, 3);
	buf[i++] = ' ';
	for (unsigned int j = 0; j < 6; j++) {
		i = apph(buf, i, mac[j], 2);
		if (j != 5)
			buf[i++] = ':';
	}
	buf[i] = '\0';
	dbg(buf);

	present = 1;
	status_line();
	return 0;
}

/* `netreset`: full re-init of an already-probed NIC (used to recover from an
 * RX stall and as a bring-up experiment). */
void r8168_reset(void)
{
	if (!present || !mmio || !rx_ring) {
		dbg("reset: not initialized");
		return;
	}
	if (hw_reset() != 0) {
		dbg("reset timeout");
		return;
	}
	read_mac();
	hw_start();
	status_line();
	dbg("netreset done");
}

void r8168_get_mac(unsigned char out[6])
{
	unsigned int i;
	for (i = 0; i < 6; i++)
		out[i] = mac[i];
}

int r8168_tx(const unsigned char *buf, unsigned int len)
{
	static unsigned char pad[60];
	struct r8168_desc *d;
	unsigned int entry, opts1, t0;
	const unsigned char *p = buf;
	unsigned int txlen = len;

	if (!present || !mmio)
		return -1;
	if (len < 14 || len > 1514)
		return -1;

	isr_poll();

	if (len < 60) {
		unsigned int i;
		for (i = 0; i < len; i++)
			pad[i] = buf[i];
		for (i = len; i < 60; i++)
			pad[i] = 0;
		p = pad;
		txlen = 60;
	}

	entry = tx_cur;
	d = &tx_ring[entry];

	/* Wait for a previous use of this slot to complete. */
	t0 = get_ticks_ms();
	while (d->opts1 & DescOwn) {
		if (timed_out(t0, 100)) {
			tx_err++;
			status_line();
			return -1;
		}
	}

	{
		unsigned char *dst = tx_bufs + entry * R8168_BUF_SIZE;
		unsigned int i;
		for (i = 0; i < txlen; i++)
			dst[i] = p[i];
	}

	opts1 = txlen | DescFirstFrag | DescLastFrag;
	if (entry == R8168_NTX - 1)
		opts1 |= DescRingEnd;
	d->addr_lo = (unsigned int)(tx_bufs + entry * R8168_BUF_SIZE);
	d->addr_hi = 0;
	d->opts2 = 0;
	dma_barrier();
	d->opts1 = opts1 | DescOwn;
	dma_barrier();

	w8(REG_TxPoll, TxPoll_NPQ);

	t0 = get_ticks_ms();
	while (d->opts1 & DescOwn) {
		if (timed_out(t0, 100)) {
			tx_err++;
			status_line();
			return -1;
		}
	}

	tx_cur = (entry + 1) % R8168_NTX;
	last_tx_status = d->opts1;
	tx_ok++;
	if ((tx_ok & 63) == 0)
		status_line();
	return 0;
}

/* Re-arm one RX descriptor (clears owner for the NIC). */
static void rx_arm(unsigned int entry)
{
	rx_ring[entry].opts2 = 0;
	dma_barrier();
	rx_ring[entry].opts1 = DescOwn | R8168_RX_ARM |
			       (entry == R8168_NRX - 1 ? DescRingEnd : 0);
	dma_barrier();
}

int r8168_rx(unsigned char *buf, unsigned int maxlen)
{
	unsigned int guard = 0;

	if (!present || !mmio)
		return -1;

	isr_poll();

	while (guard++ < 32) {
		unsigned int entry = rx_cur;
		struct r8168_desc *d = &rx_ring[entry];
		unsigned int status = d->opts1;
		unsigned int len;

		if (status & DescOwn)
			return -1;              /* nothing pending */

		dma_barrier();

		if (status & RxRES) {
			rx_err++;
		} else if ((status & (DescFirstFrag | DescLastFrag)) !=
			   (DescFirstFrag | DescLastFrag)) {
			rx_drop++;              /* fragmented frame: too big */
		} else {
			len = status & RxPktLenMask;
			if (len >= 4)
				len -= 4;       /* drop the trailing FCS */
			if (len < 14 || len > maxlen) {
				rx_drop++;
			} else {
				unsigned char *src = rx_bufs + entry * R8168_BUF_SIZE;
				unsigned int i;

				for (i = 0; i < len; i++)
					buf[i] = src[i];
				rx_ok++;
				if ((rx_ok & 63) == 0)
					status_line();
				rx_arm(entry);
				rx_cur = (rx_cur + 1) % R8168_NRX;
				return (int)len;
			}
		}

		rx_arm(entry);
		rx_cur = (rx_cur + 1) % R8168_NRX;
	}
	return -1;
}

/* PHY loopback for a self-test (driver op): transmitted frames are looped
 * back into the receiver by the PHY, so TX and RX can be validated with no
 * cable, switch or peer.  Forces 100M full duplex while enabled. */
static int saved_bmcr = -1;

int r8168_set_loopback(int on)
{
	if (!present || !mmio)
		return -1;

	if (on) {
		saved_bmcr = mdio_read(MII_BMCR);
		mdio_write(MII_BMCR, 0x2000 | 0x0100 | 0x4000);   /* 100M FD + LB */
	} else {
		int bmcr = saved_bmcr >= 0 ? (saved_bmcr & 0x7fff) : 0;
		unsigned int t0;

		mdio_write(MII_BMCR, bmcr | BMCR_AUTONEG | BMCR_RESTART_AN);
		t0 = get_ticks_ms();
		while (get_ticks_ms() - t0 < 2000) {
			if (r8(REG_PHYstatus) & PHYST_LINK)
				break;
		}
	}
	return 0;
}

void r8168_print_info(void)
{
	char sp[6];
	unsigned int st, i;

	if (!present) {
		io_putstr("r8168: no RTL8111/8168 found\n");
		return;
	}

	io_putstr(chip_name());
	io_putstr(" XID ");
	put_hex4(xid);
	io_putstr(" at ");
	put_hex2(pci_bus);
	io_putchar(':');
	put_hex2(pci_dev);
	io_putchar('.');
	io_putchar((char)('0' + pci_func));
	io_putchar('\n');

	io_putstr("  bar ");
	put_hex4(bar_base >> 16);
	put_hex4(bar_base & 0xFFFF);
	io_putstr(" (");
	put_dec(bar_size / 1024);
	io_putstr("K) mac=");
	for (i = 0; i < 6; i++) {
		if (i)
			io_putchar(':');
		put_hex2(mac[i]);
	}
	io_putchar('\n');

	st = r8(REG_PHYstatus);
	speed_string(st, sp);
	io_putstr("  link ");
	io_putstr((st & PHYST_LINK) ? "up " : "down ");
	io_putstr(sp);
	io_putstr((st & PHYST_FULLDUP) ? "F" : "H");
	io_putstr("  tx ");
	put_dec(tx_ok);
	io_putchar('/');
	put_dec(tx_err);
	io_putstr(" rx ");
	put_dec(rx_ok);
	io_putchar('/');
	put_dec(rx_err);
	io_putchar('\n');
}

void r8168_dump_regs(void)
{
	static const struct {
		const char *name;
		unsigned int reg;
	} regs[] = {
		{ "MAC0", REG_MAC0 }, { "TxDescLo", REG_TxDescLow },
		{ "TxDescHi", REG_TxDescHigh }, { "ChipCmd", REG_ChipCmd },
		{ "IntrMask", REG_IntrMask }, { "IntrStat", REG_IntrStatus },
		{ "TxConfig", REG_TxConfig }, { "RxConfig", REG_RxConfig },
		{ "PHYAR", REG_PHYAR }, { "PHYstat", REG_PHYstatus },
		{ "RxMaxSize", REG_RxMaxSize }, { "CPlusCmd", REG_CPlusCmd },
		{ "RxDescLo", REG_RxDescLow }, { "RxDescHi", REG_RxDescHigh },
	};
	unsigned int i;

	if (!present || !mmio) {
		io_putstr("r8168: no NIC\n");
		return;
	}
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		io_putstr(regs[i].name);
		io_putstr("=");
		io_puthex(r32(regs[i].reg));
		if (i & 1)
			io_putchar('\n');
		else
			io_putstr("  ");
	}
	if (i & 1)
		io_putchar('\n');
	io_putstr("state tx_ok=");
	put_dec(tx_ok);
	io_putstr(" tx_err=");
	put_dec(tx_err);
	io_putstr(" rx_ok=");
	put_dec(rx_ok);
	io_putstr(" rx_err=");
	put_dec(rx_err);
	io_putstr(" rx_drop=");
	put_dec(rx_drop);
	io_putstr(" rx_restart=");
	put_dec(rx_restarts);
	io_putchar('\n');
	io_putstr("isr ");
	if (isr_last == 0) {
		io_putstr("(none)");
	} else {
		if (isr_last & ISR_RxOK) io_putstr("RxOK ");
		if (isr_last & ISR_RxErr) io_putstr("RxErr ");
		if (isr_last & ISR_TxOK) io_putstr("TxOK ");
		if (isr_last & ISR_TxErr) io_putstr("TxErr ");
		if (isr_last & ISR_ROVW) io_putstr("ROVW ");
		if (isr_last & ISR_LinkChg) io_putstr("LinkChg ");
		if (isr_last & ISR_FOV) io_putstr("FOV ");
		if (isr_last & ISR_TDU) io_putstr("TDU ");
		if (isr_last & ISR_SWInt) io_putstr("SWInt ");
	}
	io_putstr("  evt rovw=");
	put_dec(isr_rovw);
	io_putstr(" fov=");
	put_dec(isr_fov);
	io_putstr(" tdu=");
	put_dec(isr_tdu);
	io_putchar('\n');
	io_putstr("tx status ");
	io_puthex(last_tx_status);
	io_putstr("  ring tx ");
	io_puthex((unsigned int)tx_ring);
	io_putstr(" rx ");
	io_puthex((unsigned int)rx_ring);
	io_putchar('\n');
}

const struct netdev r8168_netdev = {
	"r8168", "RTL8111/8168",
	r8168_init, r8168_get_mac, r8168_rx, r8168_tx, r8168_print_info,
	r8168_dump_regs, r8168_set_loopback, r8168_reset,
};
