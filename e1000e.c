/* Intel e1000/e1000e network driver.
 *
 * Targets the PCH LAN controllers used in Arrandale/Clarkdale laptops
 * (82577LM/82577LC, plus the 82578/82579 relations) and the plain e1000
 * (82540EM) that QEMU emulates.  The MAC/DMA path is the classic e1000
 * register set with legacy 16-byte descriptors; the PHY is reached through
 * MDIC.  Everything is polled: interrupts are masked and the ring status
 * bits are checked directly, like the other nebos NIC drivers.
 *
 * Mapped BAR0 (128 KB on the 82577LM) is above the 128 MB identity map, so
 * process_map_mmio_all() installs it in every process page directory: kernel
 * code runs with the current process's CR3 and the network syscalls follow
 * the user process.
 */
#include "e1000e.h"
#include "bootcon.h"
#include "io.h"
#include "mm.h"
#include "netdev.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

/* ---- register offsets (BAR0) ---- */
#define REG_CTRL      0x00000
#define REG_STATUS    0x00008
#define REG_EERD      0x00014
#define REG_CTRL_EXT  0x00018
#define REG_MDIC      0x00020
#define REG_PBA       0x01000
#define REG_WUC       0x05800
#define REG_VET       0x00038
#define REG_ICR       0x000C0
#define REG_IMC       0x000D8
#define REG_RCTL      0x00100
#define REG_TCTL      0x00400
#define REG_TIPG      0x00410
#define REG_RDBAL     0x02800
#define REG_RDBAH     0x02804
#define REG_RDLEN     0x02808
#define REG_RDH       0x02810
#define REG_RDT       0x02818
#define REG_TDBAL     0x03800
#define REG_TDBAH     0x03804
#define REG_TDLEN     0x03808
#define REG_TDH       0x03810
#define REG_TDT       0x03818
#define REG_TXDCTL0   0x03828
#define REG_TXDCTL1   0x03928
#define REG_TARC0     0x03840
#define REG_TARC1     0x03940
#define REG_RAL0      0x05400
#define REG_RAH0      0x05404
#define REG_GCR       0x05B00

/* GCR no-snoop enables: 1 = the NIC bypasses the CPU cache for that DMA
 * type.  A firmware that leaves the TX bits set makes the NIC read freshly
 * written TX descriptors from stale DRAM, so TX never completes (RX still
 * works because its descriptor lines were evicted long before the traffic).
 * Linux clears these on every ich8/pch part. */
#define GCR_NO_SNOOP  0x0000003Fu

/* ---- CTRL ---- */
#define CTRL_RST      0x04000000u
#define CTRL_GIO_MASTER_DISABLE 0x00000004u

/* ---- STATUS ---- */
#define STATUS_FD     0x00000001u
#define STATUS_LU     0x00000002u
#define STATUS_SPEED  0x000000C0u
#define STATUS_LAN_INIT_DONE 0x00000200u
#define STATUS_GIO_MASTER_ENABLE 0x00080000u

/* ---- RCTL ---- */
#define RCTL_EN       0x00000002u
#define RCTL_UPE      0x00000008u
#define RCTL_MPE      0x00000010u
#define RCTL_LPE      0x00000020u
#define RCTL_BAM      0x00008000u
#define RCTL_SECRC    0x04000000u

/* ---- TCTL ---- */
#define TCTL_EN       0x00000002u
#define TCTL_PSP      0x00000008u
#define TCTL_RTLC     0x01000000u
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 8
#define TCTL_CT       0x10u
#define TCTL_COLD     0x40u

/* ---- TXDCTL (PCH LAN: descriptor write-back policy) ---- */
#define TXDCTL_PTHRESH     0x0000003Fu
#define TXDCTL_WTHRESH     0x003F0000u
#define TXDCTL_COUNT_DESC  0x00400000u
#define TXDCTL_GRAN        0x01000000u
#define TXDCTL_WB_ONE      0x01010000u   /* GRAN | WTHRESH=1 */
#define TXDCTL_PREFETCH    0x0100001Fu   /* GRAN | PTHRESH=31 */
#define TXDCTL_HTHRESH     0x00000100u   /* host threshold = 1 */

/* ---- MDIC ---- */
#define MDIC_DATA_MASK 0x0000FFFFu
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_WRITE  0x04000000u
#define MDIC_OP_READ   0x08000000u
#define MDIC_READY     0x10000000u
#define MDIC_ERROR     0x40000000u

/* ---- EERD ---- */
#define EERD_START     0x00000001u
#define EERD_DONE      0x00000010u
#define EERD_ADDR_SHIFT 8
#define EERD_DATA_SHIFT 16

/* ---- descriptor status ---- */
#define RX_DD         0x01
#define RX_EOP        0x02
#define TX_DD         0x01
#define TX_CMD        (0x01u | 0x02u | 0x08u)   /* EOP | IFCS | RS */

/* ---- rings ---- */
#define E1000E_NRX    64
#define E1000E_NTX    16
#define E1000E_BUF    2048
#define E1000E_TX_TIMEOUT_MS 20
#define E1000E_TX_FAIL_MAX   32

/* ---- PHY / MII ---- */
#define MII_BMCR      0
#define MII_BMSR      1
#define MII_PHYID1    2
#define MII_PHYID2    3
#define MII_ANAR      4
#define MII_GBSR      9
#define BMCR_ANENABLE 0x1000
#define BMCR_ANRESTART 0x0200

/* ---- PCI ---- */
#define PCI_CMD_INT_DISABLE 0x0400u   /* command bit 10: INTx disabled */
#define E1000E_VENDOR 0x8086u

struct e1000e_rx_desc {
	volatile unsigned int addr_lo;
	volatile unsigned int addr_hi;
	volatile unsigned short length;
	volatile unsigned short csum;
	volatile unsigned char status;
	volatile unsigned char errors;
	volatile unsigned short special;
} __attribute__((packed));

struct e1000e_tx_desc {
	volatile unsigned int addr_lo;
	volatile unsigned int addr_hi;
	volatile unsigned int lower;
	volatile unsigned int upper;
} __attribute__((packed));

static unsigned char *mmio;
static int present;
static int inited;
static unsigned char mac[6];
static unsigned int bar_base, bar_size;
static unsigned int pci_bus, pci_dev, pci_func;
static unsigned int device_id;
static const char *detected_chip = "Intel e1000e";

static struct e1000e_rx_desc *rx_ring;
static struct e1000e_tx_desc *tx_ring;
static unsigned char *rx_bufs;
static unsigned char *tx_bufs;
static unsigned int rx_cur;
static unsigned int tx_cur;
static unsigned int tx_pending;      /* bitmask: submitted, DD not seen yet */
static unsigned int tx_fail_run;     /* consecutive TX timeouts */
static int tx_broken;                /* stop stalling the system on a dead TX */
static unsigned int tctl_fixes;      /* TCTL.EN re-asserts */

static void tx_queue_restart(void);
static int tx_ensure_enabled(void);

static int phy_addr = -1;
static int phy_id1 = -1, phy_id2 = -1;
static int pch_lan;               /* 82577/82578/82579: keep the PHY as-is */

/* Counters (also reported by `netregs`). */
static unsigned int tx_ok, tx_err, rx_ok, rx_err, rx_drop;
static unsigned int last_tx_status, last_rx_status, last_rx_errors;
static unsigned int reset_count;

static int timed_out(unsigned int start_ms, unsigned int ms)
{
	return get_ticks_ms() - start_ms > ms;
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < ms)
		;
}

/* ---- serial debug trail (the netlog mirror carries it to `make netmon`) ---- */
static void dbg(const char *s)
{
	serial_puts(SERIAL_COM1, "e1000e: ");
	serial_puts(SERIAL_COM1, s);
	serial_puts(SERIAL_COM1, "\n");
	/* Also show the trail on the boot console (NOWM diagnostics): if the
	 * probe wedges, the last line says which step.  No-op once the WM has
	 * taken over and disabled the console. */
	bootcon_puts("e1000e: ");
	bootcon_puts(s);
	bootcon_puts("\n");
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
		b[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		io_putchar(b[--i]);
}

static void put_hex2(unsigned int v)
{
	static const char hx[] = "0123456789ABCDEF";

	io_putchar(hx[(v >> 4) & 0xF]);
	io_putchar(hx[v & 0xF]);
}

static void put_mac(const unsigned char *m)
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		put_hex2(m[i]);
		if (i != 5)
			io_putchar(':');
	}
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

/* ---- PCI discovery ---- */
static int supported_device(unsigned int dev)
{
	switch (dev) {
	case 0x100E:   /* 82540EM: QEMU's e1000 */
	case 0x10D3:   /* 82574L: QEMU's e1000e */
	case 0x10EA:   /* 82577LM: ThinkPad X201 */
	case 0x10EB:   /* 82577LC */
	case 0x10EF:   /* 82578DM */
	case 0x10F0:   /* 82578DC */
	case 0x1502:   /* 82579LM */
	case 0x1503:   /* 82579V */
		return 1;
	default:
		return 0;
	}
}

static const char *chip_name(unsigned int dev)
{
	switch (dev) {
	case 0x100E: return "Intel 82540EM (QEMU e1000)";
	case 0x10D3: return "Intel 82574L";
	case 0x10EA: return "Intel 82577LM";
	case 0x10EB: return "Intel 82577LC";
	case 0x10EF: return "Intel 82578DM";
	case 0x10F0: return "Intel 82578DC";
	case 0x1502: return "Intel 82579LM";
	case 0x1503: return "Intel 82579V";
	default:     return "Intel e1000e";
	}
}

/* PCH-integrated LAN: the PHY sits behind the Kumeran interface and needs a
 * large set of PHY/Kumeran workarounds after a full-chip reset.  Booting the
 * firmware has already configured it, so the driver deliberately does a warm
 * init on these parts (stop our RX/TX, leave CTRL.PHY_RST alone) and only
 * restarts autoneg if the link is down. */
static int is_pch_lan(unsigned int dev)
{
	switch (dev) {
	case 0x10EA: case 0x10EB: case 0x10EF: case 0x10F0:
	case 0x1502: case 0x1503:
		return 1;
	default:
		return 0;
	}
}

/* BAR0 is the 128 KB register window on every supported part.  Handle a
 * 64-bit BAR and probe its size with the usual all-ones trick. */
static int probe_bar0(unsigned int bus, unsigned int dev, unsigned int func,
		      unsigned int *base, unsigned int *size)
{
	unsigned int val = pci_cfg_read32(bus, dev, func, 0x10);

	if (!val || val == 0xFFFFFFFFu || (val & 1u))
		return -1;
	/* Do NOT size the BAR by writing 0xFFFFFFFF: on PCH LAN parts the
	 * ME/AMT can leave the BAR restore incomplete, after which every MMIO
	 * access goes to an undecoded address and stalls.  All supported
	 * e1000/e1000e BARs are 128 KB, so trust the firmware-assigned base. */
	if (val & 4u)
		return -1;               /* 64-bit BAR: not used by these parts */
	*base = val & 0xFFFFFFF0u;
	*size = 0x20000;
	return 0;
}

/* Put the function in D0 before the first MMIO access.  The PCH LAN is
 * often left in D3hot by the BIOS/ME; MMIO while suspended can return
 * 0xFFFFFFFF or stall the PCIe root complex.  Walk the capability list for
 * the PM capability (ID 1) and clear PMCSR.PS. */
static void pci_set_d0(unsigned int bus, unsigned int dev, unsigned int func)
{
	unsigned int status = pci_cfg_read32(bus, dev, func, 0x04) >> 16;
	unsigned int cap, guard = 0;

	if (!(status & 0x10u))
		return;                       /* no capability list */
	cap = pci_cfg_read32(bus, dev, func, 0x34) & 0xFCu;
	while (cap && guard++ < 48) {
		unsigned int hdr = pci_cfg_read32(bus, dev, func, cap);

		if ((hdr & 0xFFu) == 0x01u) {
			unsigned int pmcsr =
				pci_cfg_read32(bus, dev, func, cap + 4);

			if (pmcsr & 3u) {
				pci_cfg_write32(bus, dev, func, cap + 4,
						pmcsr & ~3u);
				delay_ms(10);
				dbg("PM state -> D0");
			}
			return;
		}
		cap = (hdr >> 8) & 0xFCu;
	}
}

static int probe_pci(void)
{
	unsigned int bus, dev, func;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int htype, nfunc;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;

			for (func = 0; func < nfunc; func++) {
				unsigned int idf, cmd;

				idf = pci_cfg_read32(bus, dev, func, 0x00);
				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;
				if ((idf & 0xFFFFu) != E1000E_VENDOR)
					continue;
				if (!supported_device((idf >> 16) & 0xFFFFu))
					continue;

				/* Memory space + bus master. */
				cmd = pci_cfg_read32(bus, dev, func, 0x04);
				/* Polled driver: disable INTx before enabling memory
				 * and bus master.  A link-change interrupt from
				 * the PHY can otherwise fire before hw_stop()
				 * masks the device, and the kernel has no NIC IRQ
				 * handler (the X201 stalls with the cable in). */
				pci_cfg_write32(bus, dev, func, 0x04,
						cmd | 0x0006u |
						PCI_CMD_INT_DISABLE);
				pci_set_d0(bus, dev, func);
				/* Let the function settle after D0/memory enable;
				 * the ME can still be finishing its link work when
				 * the cable is present. */
				delay_ms(10);

				if (probe_bar0(bus, dev, func,
					       &bar_base, &bar_size) != 0)
					continue;

				pci_bus = bus;
				pci_dev = dev;
				pci_func = func;
				device_id = (idf >> 16) & 0xFFFFu;
				detected_chip = chip_name(device_id);
				pch_lan = is_pch_lan(device_id);
				return 0;
			}
		}
	}
	return -1;
}

/* ---- MAC address ---- */

/* Classic EERD word read; used only when RAL/RAH came back invalid. */
static int eeprom_read16(unsigned int addr, unsigned short *out)
{
	unsigned int t0, v;

	w32(REG_EERD, (addr << EERD_ADDR_SHIFT) | EERD_START);
	t0 = get_ticks_ms();
	for (;;) {
		v = r32(REG_EERD);
		if (v & EERD_DONE) {
			*out = (unsigned short)(v >> EERD_DATA_SHIFT);
			return 0;
		}
		if (timed_out(t0, 5))
			return -1;
	}
}

static int mac_valid(const unsigned char *m)
{
	unsigned int i;
	int zero = 1, ones = 1;

	for (i = 0; i < 6; i++) {
		if (m[i] != 0x00)
			zero = 0;
		if (m[i] != 0xFF)
			ones = 0;
	}
	return !zero && !ones;
}

static void read_mac(void)
{
	unsigned int ral = r32(REG_RAL0);
	unsigned int rah = r32(REG_RAH0);

	mac[0] = (unsigned char)(ral & 0xFF);
	mac[1] = (unsigned char)((ral >> 8) & 0xFF);
	mac[2] = (unsigned char)((ral >> 16) & 0xFF);
	mac[3] = (unsigned char)((ral >> 24) & 0xFF);
	mac[4] = (unsigned char)(rah & 0xFF);
	mac[5] = (unsigned char)((rah >> 8) & 0xFF);

	if (!mac_valid(mac)) {
		unsigned short w0 = 0, w1 = 0, w2 = 0;

		if (eeprom_read16(0, &w0) == 0 &&
		    eeprom_read16(1, &w1) == 0 &&
		    eeprom_read16(2, &w2) == 0) {
			mac[0] = (unsigned char)(w0 & 0xFF);
			mac[1] = (unsigned char)(w0 >> 8);
			mac[2] = (unsigned char)(w1 & 0xFF);
			mac[3] = (unsigned char)(w1 >> 8);
			mac[4] = (unsigned char)(w2 & 0xFF);
			mac[5] = (unsigned char)(w2 >> 8);
		}
	}

	/* Program the unicast filter (RAH first, then RAL on these parts).
	 * Promiscuous mode is on as well for bring-up, but a valid RAL/RAH
	 * keeps the hardware filter sane if UPE is ever turned off. */
	w32(REG_RAH0, ((unsigned int)mac[5] << 8) | mac[4]);
	w32(REG_RAL0, (unsigned int)mac[0] | ((unsigned int)mac[1] << 8) |
		      ((unsigned int)mac[2] << 16) | ((unsigned int)mac[3] << 24));
}

/* ---- PHY access via MDIC ---- */
static int mdic_read(unsigned int phy, unsigned int reg)
{
	unsigned int t0, v;

	w32(REG_MDIC, (reg << MDIC_REG_SHIFT) | (phy << MDIC_PHY_SHIFT) |
		      MDIC_OP_READ);
	t0 = get_ticks_ms();
	for (;;) {
		v = r32(REG_MDIC);
		if (v & MDIC_READY)
			return (v & MDIC_ERROR) ? -1 : (int)(v & MDIC_DATA_MASK);
		if (timed_out(t0, 10))
			return -1;
	}
}

static int mdic_write(unsigned int phy, unsigned int reg, unsigned int data)
{
	unsigned int t0, v;

	w32(REG_MDIC, (data & MDIC_DATA_MASK) | (reg << MDIC_REG_SHIFT) |
		      (phy << MDIC_PHY_SHIFT) | MDIC_OP_WRITE);
	t0 = get_ticks_ms();
	for (;;) {
		v = r32(REG_MDIC);
		if (v & MDIC_READY)
			return (v & MDIC_ERROR) ? -1 : 0;
		if (timed_out(t0, 10))
			return -1;
	}
}

static void phy_probe(void)
{
	unsigned int phy;

	phy_addr = -1;
	phy_id1 = phy_id2 = -1;
	for (phy = 0; phy < 4; phy++) {
		int id1 = mdic_read(phy, MII_PHYID1);
		int id2 = mdic_read(phy, MII_PHYID2);

		if (id1 < 0 || id2 < 0)
			continue;
		if ((id1 & 0xFFFF) == 0 && (id2 & 0xFFFF) == 0)
			continue;
		if ((id1 & 0xFFFF) == 0xFFFF)
			continue;
		phy_addr = (int)phy;
		phy_id1 = id1;
		phy_id2 = id2;
		break;
	}
}

static void phy_autoneg(void)
{
	if (phy_addr < 0)
		return;
	mdic_write(phy_addr, MII_ANAR, 0x01E1);   /* 10/100 half+full */
	mdic_write(phy_addr, MII_GBSR, 0x0300);   /* 1000 half+full */
	mdic_write(phy_addr, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

/* ---- hardware bring-up ---- */

/* Quiesce the MAC without touching the PHY: mask interrupts, stop RX/TX and
 * let any in-flight DMA finish.  This is the "reset" path on PCH LAN parts. */
static void hw_stop(void)
{
	/* PCH LAN: let the ME finish the current link transaction before the
	 * first MAC write. */
	delay_ms(5);
	w32(REG_IMC, 0xFFFFFFFFu);
	(void)r32(REG_ICR);
	w32(REG_RCTL, 0);
	w32(REG_TCTL, TCTL_PSP);
	(void)r32(REG_STATUS);       /* flush posted writes */
	delay_ms(10);
}

/* PCH LAN with AMT: the firmware (ME) must be told that the OS driver owns
 * the network interface, otherwise it can keep the LAN - notably the TX
 * path - to itself.  e1000e's e1000e_get_hw_control() sets
 * CTRL_EXT.DRV_LOAD for parts with FLAG_HAS_CTRLEXT_ON_LOAD (the pch_info
 * used by the 82577LM/82578/82579).  Do it after the reset/quiesce path and
 * before the rings are programmed, exactly where e1000e calls it. */
static void pch_claim(void)
{
	unsigned int ctrl_ext;

	if (!pch_lan)
		return;
	ctrl_ext = r32(REG_CTRL_EXT);
	if (!(ctrl_ext & 0x10000000u)) {
		w32(REG_CTRL_EXT, ctrl_ext | 0x10000000u);
		dbg("AMT handshake: CTRL_EXT.DRV_LOAD set");
	}

	/* e1000e_reset(): clear wake-up control and enable VLAN EtherType.
	 * PBA is only read/reported: writing the generic PCH default of 26
	 * would set the TX half to zero on split-PBA parts (verified on
	 * QEMU's 82540: 0x00100030 -> 0x0000001A). */
	w32(REG_WUC, 0);
	w32(REG_VET, 0x8100);
	serial_puts(SERIAL_COM1, "e1000e: pba ");
	serial_puthex(SERIAL_COM1, r32(REG_PBA));
	serial_puts(SERIAL_COM1, "\n");
}

static int hw_reset(void)
{
	unsigned int t0, ctrl;
	/* Sample the link before hw_stop(): the PHY link is independent of
	 * the MAC RX/TX state. */
	int link_up = pch_lan && (r32(REG_STATUS) & STATUS_LU);

	hw_stop();
	if (link_up) {
		/* Do not issue CTRL_RST while the ME has the link up: on the
		 * X201 the CTRL access after GIO_MASTER_DISABLE wedges.  The
		 * firmware already brought the MAC/PHY up; hw_stop() stopped
		 * RX/TX and hw_start() reprograms the queues below. */
		dbg("MAC reset skipped (link up)");
		return 0;
	}
	delay_ms(5);

	/* Block new DMA requests and wait for the last TLP to finish before
	 * hitting the MAC with a global reset (same order as e1000e). */
	ctrl = r32(REG_CTRL);
	w32(REG_CTRL, ctrl | CTRL_GIO_MASTER_DISABLE);
	t0 = get_ticks_ms();
	while ((r32(REG_STATUS) & STATUS_GIO_MASTER_ENABLE) &&
	       !timed_out(t0, 1000))
		;
	delay_ms(5);

	w32(REG_CTRL, r32(REG_CTRL) | CTRL_RST);
	t0 = get_ticks_ms();
	while (r32(REG_CTRL) & CTRL_RST) {
		if (timed_out(t0, 1000))
			return -1;
	}
	delay_ms(20);          /* NVM auto-read after reset */

	/* Wait for the NVM auto-read to finish and clear the done bit. */
	t0 = get_ticks_ms();
	while (!(r32(REG_STATUS) & STATUS_LAN_INIT_DONE) &&
	       !timed_out(t0, 200))
		;
	w32(REG_STATUS, r32(REG_STATUS) & ~STATUS_LAN_INIT_DONE);

	reset_count++;
	return 0;
}

static void setup_rings(void)
{
	unsigned int i;

	for (i = 0; i < E1000E_NRX; i++) {
		rx_ring[i].addr_lo = (unsigned int)(rx_bufs + i * E1000E_BUF);
		rx_ring[i].addr_hi = 0;
		rx_ring[i].length = 0;
		rx_ring[i].csum = 0;
		rx_ring[i].status = 0;
		rx_ring[i].errors = 0;
		rx_ring[i].special = 0;
	}
	for (i = 0; i < E1000E_NTX; i++) {
		tx_ring[i].addr_lo = (unsigned int)(tx_bufs + i * E1000E_BUF);
		tx_ring[i].addr_hi = 0;
		tx_ring[i].lower = 0;
		tx_ring[i].upper = 0;
	}
	__sync_synchronize();

	w32(REG_RDBAL, (unsigned int)rx_ring);
	w32(REG_RDBAH, 0);
	w32(REG_RDLEN, E1000E_NRX * (unsigned int)sizeof(struct e1000e_rx_desc));
	w32(REG_RDH, 0);
	w32(REG_RDT, E1000E_NRX - 1);

	w32(REG_TDBAL, (unsigned int)tx_ring);
	w32(REG_TDBAH, 0);
	w32(REG_TDLEN, E1000E_NTX * (unsigned int)sizeof(struct e1000e_tx_desc));
	w32(REG_TDH, 0);
	w32(REG_TDT, 0);

	rx_cur = 0;
	tx_cur = 0;
	tx_pending = 0;
	tx_fail_run = 0;
	tx_broken = 0;
}

/* PCH LAN: program the TX descriptor write-back policy and arbitration bits
 * that e1000e's ich8lan init sets.  Without WTHRESH/COUNT_DESC the 82577 was
 * observed to never report TX descriptor completion (tx_ok stayed 0), while
 * RX worked. */
static void tx_desc_config(void)
{
	unsigned int v, gcr;

	/* Enable snooping for TX/RX DMA (see GCR_NO_SNOOP). */
	gcr = r32(REG_GCR);
	w32(REG_GCR, gcr & ~GCR_NO_SNOOP);
	serial_puts(SERIAL_COM1, "e1000e: gcr ");
	serial_puthex(SERIAL_COM1, gcr);
	serial_puts(SERIAL_COM1, " -> ");
	serial_puthex(SERIAL_COM1, gcr & ~GCR_NO_SNOOP);
	serial_puts(SERIAL_COM1, "\n");

	/* CTRL_EXT bits e1000e's ich8 init sets: relaxed-ordering disable,
	 * the always-set bit 22, and PHY power-down in D3. */
	v = r32(REG_CTRL_EXT);
	v |= 0x00020000u | 0x00400000u | 0x00100000u;
	w32(REG_CTRL_EXT, v);

	v = r32(REG_TXDCTL0);
	v |= TXDCTL_COUNT_DESC;
	v = (v & ~TXDCTL_WTHRESH) | TXDCTL_WB_ONE;
	v = (v & ~TXDCTL_PTHRESH) | TXDCTL_PREFETCH;
	v |= TXDCTL_HTHRESH;
	w32(REG_TXDCTL0, v);

	v = r32(REG_TXDCTL1);
	v |= TXDCTL_COUNT_DESC;
	v = (v & ~TXDCTL_WTHRESH) | TXDCTL_WB_ONE;
	v = (v & ~TXDCTL_PTHRESH) | TXDCTL_PREFETCH;
	v |= TXDCTL_HTHRESH;
	w32(REG_TXDCTL1, v);

	/* TARC queue arbitration workarounds from the PCH init. */
	w32(REG_TARC0, r32(REG_TARC0) | 0x0D800000u);
	w32(REG_TARC1, r32(REG_TARC1) | 0x51000000u);
}

static void hw_start(void)
{
	setup_rings();
	w32(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
	if (pch_lan)
		tx_desc_config();
	w32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_RTLC |
		      (TCTL_CT << TCTL_CT_SHIFT) |
		      (TCTL_COLD << TCTL_COLD_SHIFT));
	w32(REG_TIPG, 10u | (8u << 10) | (6u << 20));
	__sync_synchronize();
}

/* ---- netdev interface ---- */

static int e1000e_init(void)
{
	char buf[64];
	unsigned int i;

	if (inited)
		return present ? 0 : -1;
	inited = 1;

	if (probe_pci() != 0) {
		dbg("no supported Intel NIC");
		return -1;
	}

	process_map_mmio_all(bar_base, bar_size);
	mmio = (unsigned char *)(unsigned long)bar_base;
	if (r32(REG_STATUS) == 0xFFFFFFFFu) {
		dbg("MMIO read failed");
		mmio = 0;
		return -1;
	}

	i = 0;
	/* "PCI 00:19.0 f2500000 128K" */
	{
		static const char hx[] = "0123456789ABCDEF";
		unsigned int n = 0;

		buf[i++] = 'P'; buf[i++] = 'C'; buf[i++] = 'I'; buf[i++] = ' ';
		for (n = 0; n < 2; n++)
			buf[i++] = hx[(pci_bus >> (4 * (1 - n))) & 0xF];
		buf[i++] = ':';
		for (n = 0; n < 2; n++)
			buf[i++] = hx[(pci_dev >> (4 * (1 - n))) & 0xF];
		buf[i++] = '.';
		buf[i++] = (char)('0' + pci_func);
		buf[i++] = ' ';
		for (n = 0; n < 8; n++)
			buf[i++] = hx[(bar_base >> (4 * (7 - n))) & 0xF];
		buf[i++] = ' ';
		buf[i] = '\0';
	}
	dbg(buf);

	/* The PCH LAN/ME needs a moment after the config-space setup before
	 * the first MAC register access; without it the probe can wedge when
	 * the link is up. */
	delay_ms(10);

	/* MAC-only reset: CTRL.PHY_RST is deliberately never set, so the
	 * firmware's PHY/Kumeran setup (and the link) survives, while the MAC
	 * registers - including whatever TX queue state the BIOS UNDI left
	 * behind - go back to their defaults.  e1000e does the same on parts
	 * where the ME owns the PHY. */
	if (hw_reset() != 0) {
		dbg("MAC reset timeout");
		mmio = 0;
		return -1;
	}
	dbg("reset ok");
	pch_claim();

	read_mac();

	/* All ring and buffer memory in one allocation: the page allocator is
	 * IRQ-safe but not cross-CPU locked, so two separate calls can (and
	 * once did) hand out the same page under boot-time concurrency.  A
	 * single contiguous run cannot alias itself. */
	{
		unsigned int pages = 2 +
			(E1000E_NRX * E1000E_BUF) / PAGE_SIZE +
			(E1000E_NTX * E1000E_BUF) / PAGE_SIZE;
		unsigned int mem = mm_alloc_pages(pages);

		if (mem) {
			rx_ring = (struct e1000e_rx_desc *)mem;
			tx_ring = (struct e1000e_tx_desc *)(mem + PAGE_SIZE);
			rx_bufs = (unsigned char *)(mem + 2 * PAGE_SIZE);
			tx_bufs = rx_bufs + E1000E_NRX * E1000E_BUF;
		}
	}
	serial_puts(SERIAL_COM1, "e1000e: ring mem rx=");
	serial_puthex(SERIAL_COM1, (unsigned int)rx_ring);
	serial_puts(SERIAL_COM1, " tx=");
	serial_puthex(SERIAL_COM1, (unsigned int)tx_ring);
	serial_puts(SERIAL_COM1, " rxb=");
	serial_puthex(SERIAL_COM1, (unsigned int)rx_bufs);
	serial_puts(SERIAL_COM1, " txb=");
	serial_puthex(SERIAL_COM1, (unsigned int)tx_bufs);
	serial_puts(SERIAL_COM1, "\n");
	if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) {
		dbg("no memory for rings");
		mmio = 0;
		return -1;
	}

	phy_probe();
	/* The 82577 PHY is at MDIC address 1 even when the probe could not
	 * read its ID (the PCH needs a slow-MDIO mode we do not program); keep
	 * the address so a link-down autoneg attempt can still go out. */
	if (pch_lan && phy_addr < 0)
		phy_addr = 1;
	hw_start();
	delay_ms(10);

	{
		unsigned int st = r32(REG_STATUS);

		dbg(st & STATUS_LU ? "link up" : "link started");
	}

	present = 1;

	/* "<chip> <mac>" */
	i = 0;
	for (const char *p = detected_chip; *p && i + 1 < sizeof(buf); p++)
		buf[i++] = *p;
	buf[i++] = ' ';
	for (unsigned int j = 0; j < 6 && i + 4 < sizeof(buf); j++) {
		static const char hx[] = "0123456789ABCDEF";

		buf[i++] = hx[(mac[j] >> 4) & 0xF];
		buf[i++] = hx[mac[j] & 0xF];
		if (j != 5)
			buf[i++] = ':';
	}
	buf[i] = '\0';
	dbg(buf);

	if (!(r32(REG_STATUS) & STATUS_LU))
		phy_autoneg();
	tx_ensure_enabled();
	return 0;
}

static void e1000e_get_mac(unsigned char out[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		out[i] = mac[i];
}

static int e1000e_rx(unsigned char *buf, unsigned int maxlen)
{
	unsigned int guard = 0;

	if (!present || !mmio)
		return -1;

	while (guard++ < 8) {
		unsigned int entry = rx_cur;
		struct e1000e_rx_desc *d = &rx_ring[entry];
		unsigned int status = d->status;
		unsigned int len;

		if (!(status & RX_DD))
			return -1;              /* nothing pending */
		__sync_synchronize();

		last_rx_status = status;
		last_rx_errors = d->errors;

		if ((status & RX_EOP) && d->errors == 0) {
			len = d->length;
			if (len >= 14 && len <= maxlen) {
				unsigned char *src = rx_bufs +
						    entry * E1000E_BUF;
				unsigned int i;

				for (i = 0; i < len; i++)
					buf[i] = src[i];
				rx_ok++;
				d->status = 0;
				__sync_synchronize();
				w32(REG_RDT, entry);
				rx_cur = (entry + 1) % E1000E_NRX;
				return (int)len;
			}
			rx_drop++;
		} else {
			rx_err++;
		}

		/* Drop this descriptor and give it back to the hardware. */
		d->status = 0;
		__sync_synchronize();
		w32(REG_RDT, entry);
		rx_cur = (entry + 1) % E1000E_NRX;
	}
	return -1;
}

static int tx_timeout(void)
{
	tx_err++;
	if (++tx_fail_run >= E1000E_TX_FAIL_MAX)
		tx_broken = 1;
	return -1;
}

/* The TX engine is not moving (TDH stuck): toggle TCTL.EN and re-arm the
 * ring.  Any descriptor the hardware never retired is simply dropped; the
 * network stack retries the frame.  This also makes the next TDT write a
 * real 0 -> 1 transition again, in case the first doorbell was lost while
 * the queue was disabled. */
static void tx_queue_restart(void)
{
	unsigned int tctl = r32(REG_TCTL);

	w32(REG_TCTL, tctl & ~TCTL_EN);
	delay_ms(1);
	w32(REG_TDBAL, (unsigned int)tx_ring);
	w32(REG_TDBAH, 0);
	w32(REG_TDLEN, (unsigned int)(E1000E_NTX *
				      sizeof(struct e1000e_tx_desc)));
	w32(REG_TDH, 0);
	w32(REG_TDT, 0);
	w32(REG_TCTL, tctl | TCTL_EN);
	__sync_synchronize();
	tx_cur = 0;
	tx_pending = 0;
}

/* The MAC may clear TCTL.EN on a link transition; make sure it is set before
 * a send.  Returns 1 when it had to be re-enabled. */
static int tx_ensure_enabled(void)
{
	unsigned int tctl = r32(REG_TCTL);

	if (tctl & TCTL_EN)
		return 0;
	w32(REG_TCTL, tctl | TCTL_EN);
	tctl_fixes++;
	return 1;
}

static int e1000e_tx(const unsigned char *buf, unsigned int len)
{
	static unsigned char pad[60];
	struct e1000e_tx_desc *d;
	unsigned int entry, t0;
	const unsigned char *p = buf;
	unsigned int txlen = len;

	if (!present || !mmio)
		return -1;
	/* Once TX has failed repeatedly, fail instantly: the netlog flush and
	 * the DHCP/ping loops must not hold the BKL for 100 ms per frame on a
	 * NIC whose TX does not complete.  `netreset` clears this. */
	if (tx_broken)
		return -1;
	if (len < 14 || len > 1514)
		return -1;

	tx_ensure_enabled();

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

	/* A previous command on this slot that timed out may still be in
	 * flight; wait for it before reuse. */
	if (tx_pending & (1u << entry)) {
		t0 = get_ticks_ms();
		while (!(d->upper & TX_DD)) {
			if (timed_out(t0, E1000E_TX_TIMEOUT_MS)) {
				/* The hardware never retired this
				 * descriptor: restart the TX queue and
				 * drop the slot. */
				tx_queue_restart();
				return tx_timeout();
			}
		}
		tx_pending &= ~(1u << entry);
	}

	{
		unsigned char *dst = tx_bufs + entry * E1000E_BUF;
		unsigned int i;

		for (i = 0; i < txlen; i++)
			dst[i] = p[i];
	}

	d->addr_lo = (unsigned int)(tx_bufs + entry * E1000E_BUF);
	d->addr_hi = 0;
	d->lower = txlen | (TX_CMD << 24);
	d->upper = 0;
	__sync_synchronize();
	tx_pending |= 1u << entry;

	w32(REG_TDT, (entry + 1) % E1000E_NTX);

	t0 = get_ticks_ms();
	while (!(d->upper & TX_DD)) {
		if (timed_out(t0, E1000E_TX_TIMEOUT_MS)) {
			/* Leave the pending bit set: the next TX on this slot
			 * waits for it before reuse. */
			return tx_timeout();
		}
	}
	last_tx_status = d->upper;
	tx_pending &= ~(1u << entry);
	tx_fail_run = 0;
	tx_ok++;
	tx_cur = (entry + 1) % E1000E_NTX;
	return 0;
}

static void e1000e_print_info(void)
{
	unsigned int st, bmcr = 0, bmsr = 0;

	if (!present || !mmio) {
		io_putstr("e1000e: no NIC\n");
		return;
	}

	st = r32(REG_STATUS);
	io_putstr("chip: ");
	io_putstr(detected_chip);
	io_putstr("\npci ");
	put_dec(pci_bus); io_putchar(':');
	put_dec(pci_dev); io_putchar('.');
	put_dec(pci_func);
	io_putstr(" bar0=0x");
	{
		static const char hx[] = "0123456789ABCDEF";
		unsigned int i;

		for (i = 0; i < 8; i++)
			io_putchar(hx[(bar_base >> (4 * (7 - i))) & 0xF]);
	}
	io_putstr(" mac ");
	put_mac(mac);
	io_putstr("\nlink ");
	if (st & STATUS_LU) {
		io_putstr((st & STATUS_FD) ? "up full" : "up half");
		io_putstr(" ");
		switch (st & STATUS_SPEED) {
		case 0x00: io_putstr("10M"); break;
		case 0x40: io_putstr("100M"); break;
		case 0x80: io_putstr("1000M"); break;
		default:   io_putstr("?M"); break;
		}
	} else {
		io_putstr("down");
	}
	io_putstr(" status=0x");
	io_puthex(st);
	io_putstr("\nphy ");
	if (phy_addr >= 0) {
		put_dec((unsigned int)phy_addr);
		io_putstr(" id ");
		put_hex2((unsigned int)(phy_id1 >> 8) & 0xFF);
		put_hex2((unsigned int)phy_id1 & 0xFF);
		io_putchar(':');
		put_hex2((unsigned int)(phy_id2 >> 8) & 0xFF);
		put_hex2((unsigned int)phy_id2 & 0xFF);
		bmcr = (unsigned int)mdic_read((unsigned int)phy_addr, MII_BMCR);
		bmsr = (unsigned int)mdic_read((unsigned int)phy_addr, MII_BMSR);
		io_putstr(" bmcr=0x");
		io_puthex(bmcr);
		io_putstr(" bmsr=0x");
		io_puthex(bmsr);
	} else {
		io_putstr("none");
	}
	io_putstr("txdctl=0x");
	io_puthex(r32(REG_TXDCTL0));
	io_putstr(" tarc=0x");
	io_puthex(r32(REG_TARC0));
	io_putstr(" gcr=0x");
	io_puthex(r32(REG_GCR));
	io_putstr(" pba=0x");
	io_puthex(r32(REG_PBA));
	io_putstr(" tdh=0x");
	io_puthex(r32(REG_TDH));
	io_putstr(" tdt=0x");
	io_puthex(r32(REG_TDT));
	io_putchar('\n');
	io_putstr("rx ok ");
	put_dec(rx_ok);
	io_putstr(" err ");
	put_dec(rx_err);
	io_putstr(" drop ");
	put_dec(rx_drop);
	io_putstr("  tx ok ");
	put_dec(tx_ok);
	io_putstr(" err ");
	put_dec(tx_err);
	io_putstr(" broken ");
	put_dec((unsigned int)tx_broken);
	io_putstr(" tctl_fix ");
	put_dec(tctl_fixes);
	io_putstr("  resets ");
	put_dec(reset_count);
	io_putchar('\n');
}

static void e1000e_dump_regs(void)
{
	static const struct {
		const char *name;
		unsigned int reg;
	} regs[] = {
		{ "CTRL", REG_CTRL }, { "STATUS", REG_STATUS },
		{ "CTRL_EXT", REG_CTRL_EXT }, { "EERD", REG_EERD },
		{ "MDIC", REG_MDIC }, { "ICR", REG_ICR },
		{ "PBA", REG_PBA }, { "WUC", REG_WUC },
		{ "RCTL", REG_RCTL }, { "TCTL", REG_TCTL },
		{ "TIPG", REG_TIPG }, { "RAL0", REG_RAL0 },
		{ "RAH0", REG_RAH0 }, { "RDBAL", REG_RDBAL },
		{ "RDLEN", REG_RDLEN }, { "RDH", REG_RDH },
		{ "RDT", REG_RDT }, { "TDBAL", REG_TDBAL },
		{ "TDLEN", REG_TDLEN }, { "TDH", REG_TDH },
		{ "TDT", REG_TDT }, { "TXDCTL0", REG_TXDCTL0 },
		{ "TXDCTL1", REG_TXDCTL1 }, { "TARC0", REG_TARC0 },
		{ "TARC1", REG_TARC1 }, { "GCR", REG_GCR },
	};
	unsigned int i;

	if (!present || !mmio) {
		io_putstr("e1000e: no NIC\n");
		return;
	}
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		io_putstr(regs[i].name);
		io_putchar('=');
		io_puthex(r32(regs[i].reg));
		if ((i & 1) == 1)
			io_putchar('\n');
		else
			io_putstr("  ");
	}
	if (i & 1)
		io_putchar('\n');
	io_putstr("rings rx=0x");
	io_puthex((unsigned int)rx_ring);
	io_putstr(" tx=0x");
	io_puthex((unsigned int)tx_ring);
	io_putstr(" rx_cur=");
	put_dec(rx_cur);
	io_putstr(" tx_cur=");
	put_dec(tx_cur);
	io_putstr(" tx_pending=0x");
	io_puthex(tx_pending);
	io_putchar('\n');
	io_putstr("state rx_ok=");
	put_dec(rx_ok);
	io_putstr(" rx_err=");
	put_dec(rx_err);
	io_putstr(" rx_drop=");
	put_dec(rx_drop);
	io_putstr(" tx_ok=");
	put_dec(tx_ok);
	io_putstr(" tx_err=");
	put_dec(tx_err);
	io_putstr(" tx_broken=");
	put_dec((unsigned int)tx_broken);
	io_putstr(" tx_fail_run=");
	put_dec(tx_fail_run);
	io_putstr(" tctl_fix=");
	put_dec(tctl_fixes);
	io_putstr(" reset=");
	put_dec(reset_count);
	io_putchar('\n');
	io_putstr("last rx status=0x");
	io_puthex(last_rx_status);
	io_putstr(" errors=0x");
	io_puthex(last_rx_errors);
	io_putstr(" tx status=0x");
	io_puthex(last_tx_status);
	io_putchar('\n');
	if (tx_ring) {
		io_putstr("tx0 addr=0x");
		io_puthex(tx_ring[0].addr_lo);
		io_putstr(" lower=0x");
		io_puthex(tx_ring[0].lower);
		io_putstr(" upper=0x");
		io_puthex(tx_ring[0].upper);
		io_putchar('\n');
	}
}

static void e1000e_reset(void)
{
	if (!present || !mmio || !rx_ring) {
		io_putstr("e1000e: reset: not initialized\n");
		return;
	}
	if (hw_reset() != 0) {
		io_putstr("e1000e: reset timeout\n");
		return;
	}
	pch_claim();
	read_mac();
	hw_start();
	if (!(r32(REG_STATUS) & STATUS_LU))
		phy_autoneg();
	tx_ensure_enabled();
	io_putstr("e1000e: netreset done\n");
}

const struct netdev e1000e_netdev = {
	"e1000e", "Intel e1000e",
	e1000e_init, e1000e_get_mac, e1000e_rx, e1000e_tx, e1000e_print_info,
	e1000e_dump_regs, 0, e1000e_reset,
};
