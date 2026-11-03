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
#define REG_PBS       0x01008
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
#define REG_EXTCNF_CTRL 0x00F00
#define REG_KMRNCTRLSTA 0x00034
#define REG_KABGTXD   0x03004
#define REG_RXCSUM    0x05000
#define REG_RFCTL     0x05008
#define REG_RXDCTL0   0x02828
#define REG_RXDCTL1   0x02928
#define REG_FWSM      0x05B54
#define REG_CRC_OFFSET 0x05F50
#define REG_GCR       0x05B00

/* Hardware statistics counters (read-clear).  These are the ones that tell
 * whether the PHY/link is dropping frames: CRC/alignment errors, missed
 * packets (RX FIFO full) and the broadcast counters DHCP/ARP depend on. */
#define REG_CRCERRS   0x04000
#define REG_RXERRC    0x0400C
#define REG_MPC       0x04010
#define REG_GPRC      0x04064
#define REG_BPRC      0x04068
#define REG_MPRC      0x0406C
#define REG_GPTC      0x04070
#define REG_RUC       0x04088
#define REG_BPTC      0x040F4

/* GCR no-snoop enables: 1 = the NIC bypasses the CPU cache for that DMA
 * type.  A firmware that leaves the TX bits set makes the NIC read freshly
 * written TX descriptors from stale DRAM, so TX never completes (RX still
 * works because its descriptor lines were evicted long before the traffic).
 * Linux clears these on every ich8/pch part. */
#define GCR_NO_SNOOP  0x0000003Fu

/* ---- CTRL ---- */
#define CTRL_SLU      0x00000040u
#define CTRL_RST      0x04000000u
#define CTRL_GIO_MASTER_DISABLE 0x00000004u
#define CTRL_FRCSPD   0x00000800u
#define CTRL_FRCDPX   0x00001000u

/* ---- STATUS ---- */
#define STATUS_FD     0x00000001u
#define STATUS_LU     0x00000002u
#define STATUS_SPEED  0x000000C0u
#define STATUS_LAN_INIT_DONE 0x00000200u
#define STATUS_PHYRA  0x00000400u
#define STATUS_GIO_MASTER_ENABLE 0x00080000u

/* ---- RCTL ---- */
#define RCTL_EN       0x00000002u
#define RCTL_UPE      0x00000008u
#define RCTL_MPE      0x00000010u
#define RCTL_LPE      0x00000020u
#define RCTL_BAM      0x00008000u
#define RCTL_SECRC    0x04000000u

/* ---- RXCSUM ---- */
#define RXCSUM_TUOFL  0x00000200u
#define RXCSUM_IPPCSE 0x00001000u

/* ---- RFCTL ---- */
#define RFCTL_NFSW_DIS 0x00000040u
#define RFCTL_NFSR_DIS 0x00000080u
#define RFCTL_EXTEN    0x00008000u
#define RFCTL_IPV6_EX_DIS 0x00010000u
#define RFCTL_NEW_IPV6_EXT_DIS 0x00020000u

/* ---- TCTL ---- */
#define TCTL_EN       0x00000002u
#define TCTL_PSP      0x00000008u
#define TCTL_RTLC     0x01000000u
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12
#define TCTL_CT       0x0Fu
#define TCTL_COLD     0x3Fu
#define TCTL_CT_MASK  0x00000FF0u
#define TCTL_COLD_MASK 0x003FF000u

/* ---- TXDCTL (PCH LAN: descriptor write-back policy) ---- */
#define TXDCTL_PTHRESH     0x0000003Fu
#define TXDCTL_WTHRESH     0x003F0000u
#define TXDCTL_COUNT_DESC  0x00400000u
#define TXDCTL_GRAN        0x01000000u
#define TXDCTL_WB_ONE      0x01010000u   /* GRAN | WTHRESH=1 */
#define TXDCTL_PREFETCH    0x0100001Fu   /* GRAN | PTHRESH=31 */
#define TXDCTL_HTHRESH     0x00000100u   /* host threshold = 1 */

/* ---- RXDCTL (PCH LAN: descriptor write-back policy) ---- */
#define RXDCTL_PTHRESH     0x0000003Fu
#define RXDCTL_HTHRESH     0x00003F00u
#define RXDCTL_WTHRESH     0x003F0000u
#define RXDCTL_GRAN        0x01000000u

/* ---- MDIC ---- */
#define MDIC_TIMEOUT_MS 100   /* Linux polls up to ~96 ms; slow MDIO needs it */
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
static int last_link;                /* last STATUS.LU sampled, PCH only */
static unsigned int link_poll_ms;
static int tx_timeout_logged;

static void tx_queue_restart(void);
static int tx_ensure_enabled(void);
static void phy_autoneg(void);
static int pch_swflag_acquire(void);
static void pch_swflag_release(void);

static int phy_addr = -1;
static int phy_id1 = -1, phy_id2 = -1;
static int pch_lan;               /* 82577/82578/82579: PCH LAN/Kumeran PHY */
static int force_speed;           /* 0 = auto, 100 = advertise 100M only */

/* Counters (also reported by `netregs`). */
static unsigned int tx_ok, tx_err, rx_ok, rx_err, rx_drop;
static unsigned int rx_ucast, rx_bcast, rx_mcast;
static unsigned char last_rx_dst[6];
static unsigned int last_rx_len;
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

/* Some network syscalls enter with interrupts off; get_ticks_ms() would be
 * frozen there, so the link workarounds (which use delay_ms and MDIC
 * timeouts) must only run in a context where the PIT is ticking. */
static int irqs_on(void)
{
	unsigned int flags;

	__asm__ volatile ("pushfl; popl %0" : "=r"(flags));
	return (flags & 0x200u) != 0;
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

/* "name=0x12345678" through dbg(), so the value is visible on the boot
 * console too (make nowm) and not only on serial/netlog. */
static void dbg_hex(const char *name, unsigned int v)
{
	static const char hx[] = "0123456789ABCDEF";
	char buf[48];
	unsigned int i = 0;
	int shift;

	while (*name && i < 24)
		buf[i++] = *name++;
	buf[i++] = '=';
	buf[i++] = '0';
	buf[i++] = 'x';
	for (shift = 28; shift >= 0; shift -= 4)
		buf[i++] = hx[(v >> shift) & 0xF];
	buf[i] = '\0';
	dbg(buf);
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

/* Snapshot the registers firmware/ME could have changed.  This is the first
 * thing to look at on a board where DHCP fails: RFCTL.EXTEN in particular
 * changes the RX descriptor layout. */
static void log_state(const char *tag)
{
	dbg(tag);
	dbg_hex("  ctrl", r32(REG_CTRL));
	dbg_hex("  ctrl_ext", r32(REG_CTRL_EXT));
	dbg_hex("  status", r32(REG_STATUS));
	dbg_hex("  rfctl", r32(REG_RFCTL));
	dbg_hex("  rxcsum", r32(REG_RXCSUM));
	dbg_hex("  rxdctl", r32(REG_RXDCTL0));
	dbg_hex("  tctl", r32(REG_TCTL));
	dbg_hex("  txdctl", r32(REG_TXDCTL0));
	dbg_hex("  pba", r32(REG_PBA));
	dbg_hex("  pbs", r32(REG_PBS));
	dbg_hex("  extcnf", r32(REG_EXTCNF_CTRL));
	dbg_hex("  fwsm", r32(REG_FWSM));
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

/* The PHY workarounds below are the pchlan (82577/82578) ones.  The 82579
 * is pch2lan and has a different K1/LV sequence; leave its firmware setup
 * alone rather than writing 82577-specific registers to it. */
static int pch_phy_82577(void)
{
	return device_id == 0x10EA || device_id == 0x10EB ||
	       (phy_id1 == 0x0154 && phy_id2 >= 0 &&
		(phy_id2 & 0xFFF0) == 0x0050);
}

static int pch_phy_82578(void)
{
	return device_id == 0x10EF || device_id == 0x10F0 ||
	       phy_id1 == 0x004D;
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

/* The e1000e PCH LAN is sensitive to PCIe ASPM L1/L0s: it causes long
 * wake-up latency and packet loss on ThinkPads (the 82573/82577 family).
 * Linux's e1000e_disable_aspm() clears the ASPM enables in the Link
 * Control register for the affected parts.  On bus 0 there is no upstream
 * bridge device, so clearing the function's own enables is the same thing
 * the Linux driver does. */
static void pci_disable_aspm(unsigned int bus, unsigned int dev,
			     unsigned int func)
{
	unsigned int status = pci_cfg_read32(bus, dev, func, 0x04) >> 16;
	unsigned int cap, guard = 0;

	if (!(status & 0x10u))
		return;                       /* no capability list */
	cap = pci_cfg_read32(bus, dev, func, 0x34) & 0xFCu;
	while (cap && guard++ < 48) {
		unsigned int hdr = pci_cfg_read32(bus, dev, func, cap);

		if ((hdr & 0xFFu) == 0x10u) {   /* PCI Express capability */
			unsigned int lc =
				pci_cfg_read32(bus, dev, func,
					       cap + 0x10) & 0xFFFFu;

			if (lc & 0x3u) {
				dbg_hex("  PCIe linkctl before", lc);
				pci_cfg_write16(bus, dev, func, cap + 0x10,
						(unsigned short)(lc & ~0x3u));
				dbg_hex("  PCIe linkctl after",
					pci_cfg_read32(bus, dev, func,
						       cap + 0x10) & 0xFFFFu);
				dbg("PCIe ASPM disabled");
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
				pci_disable_aspm(bus, dev, func);
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

/* Program the unicast filter the way Linux's e1000e_rar_set_generic()
 * does: RAL first, then RAH with AV last.  The previous RAH-then-RAL
 * order could leave the exact-match filter pointing at a stale low half
 * while AV was already set; broadcast/multicast still worked, but unicast
 * to the board's own MAC was dropped. */
static void write_rar(void)
{
	w32(REG_RAL0, (unsigned int)mac[0] | ((unsigned int)mac[1] << 8) |
		      ((unsigned int)mac[2] << 16) | ((unsigned int)mac[3] << 24));
	__sync_synchronize();
	w32(REG_RAH0, 0x80000000u | ((unsigned int)mac[5] << 8) | mac[4]);
	__sync_synchronize();
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

	write_rar();
}

/* Runtime MAC override (netmac diagnostic).  The TX source, ARP and DHCP
 * client all use the `mac` array, so changing it and reprogramming RAL/RAH
 * is enough to move the whole interface to a different address. */
static void e1000e_set_mac(const unsigned char m[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		mac[i] = m[i];
	write_rar();
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
		if (timed_out(t0, pch_lan ? MDIC_TIMEOUT_MS : 10))
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
		if (timed_out(t0, pch_lan ? MDIC_TIMEOUT_MS : 10))
			return -1;
	}
}

/* ---- PCH (82577/82578/82579) PHY access ---- */

/* The HV registers are reached through the IGP page-select register (31):
 * the page number is shifted left by 5, exactly like PHY_REG(page, reg). */
/* The 82577 uses two MDIC addresses: page 0 / standard MII registers at
 * address 2, and the HV pages (768+) at address 1.  Linux's
 * e1000_get_phy_addr_for_hv_page() encodes exactly this. */
#define HV_PHY_ADDR 1
#define HV_PAGE_START 768

static unsigned int phy_page_addr(unsigned int page)
{
	if (page >= HV_PAGE_START)
		return HV_PHY_ADDR;
	return (phy_addr >= 0) ? (unsigned int)phy_addr : 0xFFFFFFFFu;
}

static void phy_set_page(unsigned int page)
{
	unsigned int addr = phy_page_addr(page);

	if (addr == 0xFFFFFFFFu)
		return;
	mdic_write(addr, 31, page << 5);
}

static int phy_read_page(unsigned int page, unsigned int reg)
{
	unsigned int addr = phy_page_addr(page);

	if (addr == 0xFFFFFFFFu)
		return -1;
	mdic_write(addr, 31, page << 5);
	return mdic_read(addr, reg);
}

static int phy_write_page(unsigned int page, unsigned int reg, unsigned int data)
{
	unsigned int addr = phy_page_addr(page);

	if (addr == 0xFFFFFFFFu)
		return -1;
	mdic_write(addr, 31, page << 5);
	return mdic_write(addr, reg, data);
}

/* e1000_set_mdio_slow_mode_hv(): the 82577 PHY needs slow MDIO timing, and
 * Linux sets it before the first reliable PHY ID read.  Without it the
 * 82577 often answers with 0/0xffff (which is why phy_probe() used to fail). */
static void pch_slow_mdio(void)
{
	int v;
	int lock;

	if (phy_addr < 0)
		return;
	lock = pch_swflag_acquire() == 0;
	v = phy_read_page(769, 16);
	if (v < 0) {
		/* The first read can fail before the PHY is in slow mode; try
		 * once more, then set the bit without a read-modify-write. */
		delay_ms(1);
		v = phy_read_page(769, 16);
	}
	if (v >= 0)
		phy_write_page(769, 16, (unsigned int)v | 0x0400u);
	else
		phy_write_page(769, 16, 0x0400u);
	phy_set_page(0);
	if (lock)
		pch_swflag_release();
}

/* e1000_hv_phy_workarounds_ich8lan() and e1000_copper_link_setup_82577().
 * The firmware normally leaves the PHY in a usable state, but these are the
 * parts that make the link reliable at gigabit speed; they are idempotent. */
static void pch_phy_workarounds(void)
{
	int v;
	int is77 = pch_phy_82577();
	int is78 = pch_phy_82578();
	int lock;

	if (!pch_lan || phy_addr < 0)
		return;
	if (!is77 && !is78)
		return;
	lock = pch_swflag_acquire() == 0;

	/* BM_PORT_GEN_CFG: clear the host wakeup bits in the low byte. */
	v = phy_read_page(769, 17);
	if (v >= 0)
		phy_write_page(769, 17, (unsigned int)v & 0x00FFu);

	/* Back to page 0 for the standard registers and the EMI window. */
	phy_set_page(0);

	/* 82577 MSE threshold: keep the link up in noise (EMI 0x0887). */
	mdic_write((unsigned int)phy_addr, 16, 0x0887);
	mdic_write((unsigned int)phy_addr, 17, 0x0034);

	if (is77) {
		/* Assert CRS on TX and enable downshift. */
		v = mdic_read((unsigned int)phy_addr, 22);
		if (v >= 0)
			mdic_write((unsigned int)phy_addr, 22,
				   (unsigned int)v | 0x8000u | 0x0C00u);

		/* Auto MDI/MDIX. */
		v = mdic_read((unsigned int)phy_addr, 18);
		if (v >= 0) {
			unsigned int mdix = (unsigned int)v;

			mdix &= ~0x0600u;
			mdix |= 0x0400u;
			mdic_write((unsigned int)phy_addr, 18, mdix);
		}

		/* Auto master/slave resolution. */
		v = mdic_read((unsigned int)phy_addr, MII_GBSR);
		if (v >= 0)
			mdic_write((unsigned int)phy_addr, MII_GBSR,
				   (unsigned int)v & ~0x1000u);
	}

	/* 82577 rev 1/2: disable early preamble generation and tune the
	 * Kumeran FIFO preamble for SSC. */
	if (is77 && phy_id1 == 0x0154 && phy_id2 >= 0 &&
	    (phy_id2 & 0xF) >= 1 && (phy_id2 & 0xF) <= 2) {
		phy_write_page(769, 25, 0x4431);
		phy_write_page(770, 16, 0xA204);
	}

	phy_set_page(0);
	if (lock)
		pch_swflag_release();
}

/* Kumeran control/status register (MAC CSR 0x34).  offset 7 bit 1 is the K1
 * enable; offset 4/9 are the timeout and in-band parameters. */
static unsigned int kmrn_read(unsigned int off)
{
	w32(REG_KMRNCTRLSTA, (off << 16) | 0x00200000u);
	delay_ms(1);
	return r32(REG_KMRNCTRLSTA) & 0xFFFFu;
}

static void kmrn_write(unsigned int off, unsigned int data)
{
	w32(REG_KMRNCTRLSTA, (off << 16) | (data & 0xFFFFu));
	delay_ms(1);
}

/* e1000_configure_k1_ich8lan(): change the K1 enable bit, then toggle the
 * forced-speed/bypass bits so the MAC samples the new setting. */
static void pch_k1_config(int enable)
{
	unsigned int v = kmrn_read(7);
	unsigned int ctrl, ctrl_ext, forced;

	if (enable)
		v |= 0x0002u;
	else
		v &= ~0x0002u;
	kmrn_write(7, v);

	ctrl = r32(REG_CTRL);
	ctrl_ext = r32(REG_CTRL_EXT);
	forced = ctrl & ~(0x00000100u | 0x00000200u);
	forced |= 0x00000800u;                /* CTRL_FRCSPD */
	w32(REG_CTRL, forced);
	w32(REG_CTRL_EXT, ctrl_ext | 0x00008000u);  /* SPD_BYPS */
	delay_ms(1);
	w32(REG_CTRL, ctrl);
	w32(REG_CTRL_EXT, ctrl_ext);
	delay_ms(1);
}

/* e1000_k1_gig_workaround_hv(): at 1 Gbps K1 must be off or the MAC can
 * stall when it transitions from a lower speed.  The preamble workaround
 * below is e1000_check_for_copper_link_ich8lan()'s half-duplex fix. */
static void pch_link_up_workarounds(void)
{
	unsigned int st = r32(REG_STATUS);
	int ms, v;
	int is77 = pch_phy_82577();
	int is78 = pch_phy_82578();
	int lock;
	int speed1000 = (st & STATUS_SPEED) == 0x80;

	if (!pch_lan || phy_addr < 0)
		return;
	if (!is77 && !is78)
		return;

	lock = pch_swflag_acquire() == 0;
	phy_set_page(0);
	if (is78) {
		/* BM_CS_STATUS: link up + resolved + 1000 Mbps. */
		ms = mdic_read((unsigned int)phy_addr, 17);
		if (ms >= 0 &&
		    (ms & 0x0C00) == 0x0C00 &&
		    (ms & 0xC000) == 0x8000)
			v = 0;
		else if (speed1000)
			v = 0;      /* STATUS says 1G: never leave K1 on */
		else
			v = 1;
	} else {
		ms = mdic_read((unsigned int)phy_addr, 26);
		if (ms >= 0 &&
		    (ms & 0x0040) && (ms & 0x1000) &&
		    (ms & 0x0300) == 0x0200)
			v = 0;
		else if (speed1000)
			v = 0;      /* PHY read failed; trust STATUS */
		else
			v = 1;
	}
	dbg(v ? "K1 enabled" : "K1 disabled (1G)");

	/* Link-stall fix, link-up value, then the K1 setting. */
	phy_write_page(770, 19, 0x0100);
	pch_k1_config(v);

	{
		int pre = phy_read_page(770, 16);

		if (pre >= 0) {
			unsigned int p = (unsigned int)pre & ~0x7000u;

			if (!(st & STATUS_FD))
				p |= 0x1000;
			phy_write_page(770, 16, p);
		}
	}
	phy_set_page(0);
	if (lock)
		pch_swflag_release();
}

static void pch_link_down_workarounds(void)
{
	int lock;

	if (!pch_lan || phy_addr < 0)
		return;
	if (!pch_phy_82577() && !pch_phy_82578())
		return;
	lock = pch_swflag_acquire() == 0;
	phy_write_page(770, 19, 0x4100);
	pch_k1_config(1);
	phy_set_page(0);
	if (lock)
		pch_swflag_release();
}

/* Poll the PHY link while commands are running.  Linux's watchdog does the
 * same every couple of seconds: re-apply the K1/preamble workarounds when
 * the link comes up and re-enable RCTL/TCTL.  Without this a cable plugged
 * in after boot never gets the link-up workarounds. */
static void link_check(void)
{
	unsigned int now, st;

	if (!present || !mmio || !pch_lan)
		return;
	if (!irqs_on())
		return;
	now = get_ticks_ms();
	if (now - link_poll_ms < 250)
		return;
	link_poll_ms = now;

	st = r32(REG_STATUS);
	if (!!(st & STATUS_LU) == last_link)
		return;
	last_link = !!(st & STATUS_LU);

	if (last_link) {
		dbg("link up (poll)");
		pch_link_up_workarounds();
		w32(REG_CTRL, r32(REG_CTRL) | CTRL_SLU);
		w32(REG_RCTL, r32(REG_RCTL) | RCTL_EN);
		w32(REG_TCTL, r32(REG_TCTL) | TCTL_EN);
	} else {
		dbg("link down (poll)");
		pch_link_down_workarounds();
		phy_autoneg();
	}
}

static void phy_probe(void)
{
	unsigned int phy;
	int lock;

	phy_addr = -1;
	phy_id1 = phy_id2 = -1;
	lock = pch_swflag_acquire() == 0;
	for (phy = 0; phy < 4; phy++) {
		int id1, id2;

		/* Make sure a previous page-select is not in effect. */
		mdic_write(phy, 31, 0);
		id1 = mdic_read(phy, MII_PHYID1);
		id2 = mdic_read(phy, MII_PHYID2);

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
	if (lock)
		pch_swflag_release();
}

/* Poll STATUS.LU for a link; used after a full-chip reset and by net100.
 * Returns 1 when the link is up, 0 on timeout. */
static int wait_link(unsigned int timeout_ms)
{
	unsigned int t0 = get_ticks_ms();

	while (!(r32(REG_STATUS) & STATUS_LU)) {
		if (timed_out(t0, timeout_ms))
			return 0;
		delay_ms(10);
	}
	return 1;
}

static void phy_autoneg(void)
{
	int lock;

	if (phy_addr < 0)
		return;
	lock = pch_swflag_acquire() == 0;
	phy_set_page(0);
	mdic_write(phy_addr, MII_ANAR, 0x01E1);   /* 10/100 half+full */
	/* force_speed is the net100 fallback: with 1000BASE-T advertisement
	 * cleared the PHY can only resolve 10/100, which bypasses a marginal
	 * 1000M pair/jack while still giving a fully usable network. */
	mdic_write(phy_addr, MII_GBSR, force_speed == 100 ? 0x0000 : 0x0300);
	mdic_write(phy_addr, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
	if (lock)
		pch_swflag_release();
}

/* net100 fallback: 0 = auto/1G advertisement, 100 = 100M only.
 *
 * The link is deliberately dropped and renegotiated.  The caller (the
 * net100 command) waits for the result; the force_speed flag is also
 * honoured by phy_autoneg() so a link-down poll cannot silently restore
 * 1000M. */
static int e1000e_set_speed(unsigned int mbps)
{
	unsigned int t0;

	if (!present || !mmio || phy_addr < 0)
		return -1;
	if (mbps != 0 && mbps != 100)
		return -1;

	force_speed = (mbps == 100) ? 100 : 0;
	phy_autoneg();        /* honours force_speed and restarts autoneg */

	/* First let the old link drop (the PHY restarts autoneg), then wait
	 * for the new one.  A switch can take a couple of seconds. */
	t0 = get_ticks_ms();
	while ((r32(REG_STATUS) & STATUS_LU) && !timed_out(t0, 1000))
		delay_ms(10);

	if (!wait_link(5000)) {
		last_link = 0;
		return -3;
	}
	if (pch_lan)
		pch_link_up_workarounds();
	/* The MAC may clear RCTL/TCTL on the link transition; re-assert them
	 * here because last_link is set to 1 and link_check() will not. */
	w32(REG_CTRL, r32(REG_CTRL) | CTRL_SLU);
	w32(REG_RCTL, r32(REG_RCTL) | RCTL_EN);
	w32(REG_TCTL, r32(REG_TCTL) | TCTL_EN);
	last_link = 1;
	return 0;
}

/* ---- hardware bring-up ---- */

/* Quiesce the MAC without touching the PHY: mask interrupts, stop RX/TX and
 * let any in-flight DMA finish.  This is the "reset" path on PCH LAN parts. */
static void hw_stop(void)
{
	unsigned int tctl;

	/* PCH LAN: let the ME finish the current link transaction before the
	 * first MAC write. */
	delay_ms(5);
	w32(REG_IMC, 0xFFFFFFFFu);
	(void)r32(REG_ICR);
	w32(REG_RCTL, 0);
	/* Stop TX without clobbering the 82577's reserved TCTL bits (bit 28
	 * and RRTHRESH are reset to 1/01 and must stay that way). */
	tctl = r32(REG_TCTL);
	w32(REG_TCTL, (tctl & ~TCTL_EN) | TCTL_PSP);
	(void)r32(REG_STATUS);       /* flush posted writes */
	delay_ms(10);
}

/* The 82577 PBA register has a 5-bit RX allocation (RXA) and a read-only
 * TX allocation (TXA = PBS - RXA).  Linux uses 26 KB RX on PCH parts,
 * leaving the rest of the packet buffer for TX.  If firmware/ME left RXA at
 * PBS, TXA is zero and the transmit engine has no FIFO space, so the queue
 * never moves. */
static void pch_pba_config(void)
{
	unsigned int pbs, pba;

	if (!pch_lan)
		return;
	pbs = r32(REG_PBS);
	pba = r32(REG_PBA);
	if (pbs > 26 && (pba & 0x1Fu) != 26) {
		w32(REG_PBA, 26);
		dbg("PBA set to 26K RX");
	}
	serial_puts(SERIAL_COM1, "e1000e: pbs ");
	serial_puthex(SERIAL_COM1, pbs);
	serial_puts(SERIAL_COM1, " pba ");
	serial_puthex(SERIAL_COM1, r32(REG_PBA));
	serial_puts(SERIAL_COM1, "\n");
}

/* PCH LAN with AMT: the firmware (ME) must be told that the OS driver owns
 * the network interface, otherwise it can keep the LAN - notably the TX
 * path - to itself.  e1000e's e1000e_get_hw_control() sets
 * CTRL_EXT.DRV_LOAD for parts with FLAG_HAS_CTRLEXT_ON_LOAD (the pch_info
 * used by the 82577LM/82578/82579).  e1000e_open() takes ownership before
 * the reset on AMT parts; do the same, then call this again after the reset
 * to restore WUC/VET. */
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
	 * PBA needs the special PCH encoding (RXA only); pch_pba_config()
	 * handles it and leaves non-PCH split-PBA parts alone. */
	w32(REG_WUC, 0);
	w32(REG_VET, 0x8100);
	pch_pba_config();

	/* These are normally applied in the post-reset path; do them here as
	 * well so a warm boot (MAC reset skipped while the link is up) gets the
	 * same values.  CRC_OFFSET is pchlan-only (0x5F50 is the receive-address
	 * CRC register on the 82579); KABGTXD applies to the whole ich8lan
	 * family. */
	if (device_id == 0x10EA || device_id == 0x10EB ||
	    device_id == 0x10EF || device_id == 0x10F0)
		w32(REG_CRC_OFFSET, 0x65656565u);
	if (pch_lan) {
		unsigned int st = r32(REG_STATUS);

		w32(REG_KABGTXD, r32(REG_KABGTXD) | 0x00050000u);
		/* Linux clears this sticky "PHY reset asserted" bit after a
		 * reset; the firmware leaves it set on the warm path. */
		if (st & STATUS_PHYRA)
			w32(REG_STATUS, st & ~STATUS_PHYRA);
	}
}

static void pch_swflag_release(void)
{
	if (pch_lan)
		w32(REG_EXTCNF_CTRL, r32(REG_EXTCNF_CTRL) & ~0x20u);
}

/* e1000_acquire_swflag_ich8lan(): coordinate PHY/MAC access with the ME
 * before a global reset.  Without this the X201 could wedge on CTRL_RST
 * while the ME was holding the shared resource. */
static int pch_swflag_acquire(void)
{
	unsigned int t0;

	if (!pch_lan)
		return -1;
	t0 = get_ticks_ms();
	while ((r32(REG_EXTCNF_CTRL) & 0x20u) && !timed_out(t0, 100))
		delay_ms(1);
	if (r32(REG_EXTCNF_CTRL) & 0x20u) {
		dbg("SWFLAG busy");
		return -1;
	}

	w32(REG_EXTCNF_CTRL, r32(REG_EXTCNF_CTRL) | 0x20u);
	t0 = get_ticks_ms();
	while (!(r32(REG_EXTCNF_CTRL) & 0x20u)) {
		if (timed_out(t0, 200)) {
			dbg("SWFLAG acquire timeout");
			w32(REG_EXTCNF_CTRL,
			    r32(REG_EXTCNF_CTRL) & ~0x20u);
			return -1;
		}
		delay_ms(1);
	}
	return 0;
}

static int hw_reset(int force)
{
	unsigned int t0, ctrl;
	/* Sample the link before hw_stop(): the PHY link is independent of
	 * the MAC RX/TX state. */
	int link_up = pch_lan && (r32(REG_STATUS) & STATUS_LU);
	int swflag = 0;

	if (link_up && !force) {
		/* netreset path only.  The probe deliberately stays on this
		 * warm path: a full PHY+MAC reset wedges the X201 PCH before
		 * the e1000e log can be flushed. */
		hw_stop();
		dbg("MAC reset skipped (link up)");
		return 0;
	}

	/* Block new DMA requests before clearing RCTL.  Linux disables the
	 * PCIe master first; clearing RCTL while DMA is active can wedge the
	 * receive packet buffer on these parts. */
	ctrl = r32(REG_CTRL);
	w32(REG_CTRL, ctrl | CTRL_GIO_MASTER_DISABLE);
	t0 = get_ticks_ms();
	while ((r32(REG_STATUS) & STATUS_GIO_MASTER_ENABLE) &&
	       !timed_out(t0, 1000))
		;
	delay_ms(5);

	hw_stop();
	delay_ms(5);

	/* Coordinate with the ME before the global reset (Linux takes the
	 * SW flag after quiescing the queues, just before CTRL_RST). */
	if (pch_lan)
		swflag = pch_swflag_acquire() == 0;

	/* MAC-only reset: CTRL.PHY_RST is deliberately never set here.  The
	 * X201 PCH wedges when the LCD/PHY reset is combined with the MAC
	 * reset, so the PHY state is left to the BIOS/ME and net100. */
	ctrl = r32(REG_CTRL);
	dbg("full reset: MAC only");
	w32(REG_CTRL, ctrl | CTRL_RST);
	t0 = get_ticks_ms();
	while (r32(REG_CTRL) & CTRL_RST) {
		if (timed_out(t0, 1000)) {
			if (swflag)
				pch_swflag_release();
			return -1;
		}
	}
	delay_ms(20);          /* NVM auto-read after reset */

	/* Linux deliberately does not write EXTCNF_CTRL.SWFLAG after a global
	 * reset: the reset clears it, and clearing it again can steal the
	 * flag from the ME.  Only the timeout/error paths release it. */

	/* Wait for the NVM auto-read to finish and clear the done bit. */
	t0 = get_ticks_ms();
	while (!(r32(REG_STATUS) & STATUS_LAN_INIT_DONE) &&
	       !timed_out(t0, 200))
		;
	w32(REG_STATUS, r32(REG_STATUS) & ~STATUS_LAN_INIT_DONE);

	if (device_id == 0x10EA || device_id == 0x10EB ||
	    device_id == 0x10EF || device_id == 0x10F0) {
		/* e1000e_reset_hw_ich8lan(): with this value noise on the
		 * Kumeran interface is detected as a CRC error and dropped
		 * before it reaches the DMA engine.  This is the pchlan
		 * (82577/82578) register; on the 82579 the same offset is
		 * the receive-address CRC calculation register. */
		w32(REG_CRC_OFFSET, 0x65656565u);
	}
	if (pch_lan)
		w32(REG_KABGTXD, r32(REG_KABGTXD) | 0x00050000u);

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
	tx_timeout_logged = 0;
	rx_ok = rx_err = rx_drop = 0;
	rx_ucast = rx_bcast = rx_mcast = 0;
	last_rx_len = 0;
	{
		unsigned int i;

		for (i = 0; i < 6; i++)
			last_rx_dst[i] = 0;
	}
	tx_ok = tx_err = 0;
	last_tx_status = 0;
	last_rx_status = 0;
	last_rx_errors = 0;
	tctl_fixes = 0;
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
	v &= ~(TXDCTL_PTHRESH | TXDCTL_HTHRESH | TXDCTL_WTHRESH);
	v |= TXDCTL_WB_ONE | 0x1Fu;   /* GRAN, WTHRESH=1, PTHRESH=31 */
	w32(REG_TXDCTL0, v);

	v = r32(REG_TXDCTL1);
	v |= TXDCTL_COUNT_DESC;
	v &= ~(TXDCTL_PTHRESH | TXDCTL_HTHRESH | TXDCTL_WTHRESH);
	v |= TXDCTL_WB_ONE | 0x1Fu;
	w32(REG_TXDCTL1, v);

	/* TARC queue arbitration workarounds from the PCH init.  Linux sets
	 * TARC(0) bits 23/24/26/27 and TARC(1) bits 24/26/30 plus bit 28
	 * when TCTL.MULR is clear (which it is here). */
	w32(REG_TARC0, r32(REG_TARC0) | 0x0D800000u);
	w32(REG_TARC1, r32(REG_TARC1) | 0x55000000u);
}

/* Pin the state of the receive path.  The firmware/ME can leave RFCTL,
 * RXCSUM and RXDCTL in whatever its driver used, and hw_reset() deliberately
 * skips the MAC reset while the link is up.  This driver uses the legacy
 * 16-byte RX descriptor, so RFCTL.EXTEN must be clear: with EXTEN set the
 * 82577 writes the extended layout (status/error at 8, length at 12) and
 * overwrites descriptor bytes 0-7 with RSS/checksum data, which the legacy
 * struct would misread as status/length. */
static void rx_desc_config(void)
{
	unsigned int rfctl = r32(REG_RFCTL);
	unsigned int rxcsum = r32(REG_RXCSUM);
	unsigned int v;

	rfctl &= ~RFCTL_EXTEN;
	rfctl |= RFCTL_NFSW_DIS | RFCTL_NFSR_DIS |
		 RFCTL_IPV6_EX_DIS | RFCTL_NEW_IPV6_EXT_DIS;
	w32(REG_RFCTL, rfctl);

	/* No checksum offload: the stack verifies checksums, and the driver
	 * rejects any descriptor whose error byte is non-zero.  With RXCSUM
	 * enabled a UDP checksum of zero (common in DHCP replies) can be
	 * reported as TCPE and the reply would be dropped. */
	w32(REG_RXCSUM, 0);

	/* Write descriptors back promptly (WTHRESH=0), prefetch 31, host
	 * threshold 1; otherwise firmware-set thresholds can delay DD. */
	v = r32(REG_RXDCTL0);
	v &= ~(RXDCTL_PTHRESH | RXDCTL_HTHRESH | RXDCTL_WTHRESH);
	v |= RXDCTL_GRAN | 0x00000100u | 0x1Fu;
	w32(REG_RXDCTL0, v);

	v = r32(REG_RXDCTL1);
	v &= ~(RXDCTL_PTHRESH | RXDCTL_HTHRESH | RXDCTL_WTHRESH);
	v |= RXDCTL_GRAN | 0x00000100u | 0x1Fu;
	w32(REG_RXDCTL1, v);

	serial_puts(SERIAL_COM1, "e1000e: rfctl ");
	serial_puthex(SERIAL_COM1, rfctl);
	serial_puts(SERIAL_COM1, " rxcsum ");
	serial_puthex(SERIAL_COM1, rxcsum);
	serial_puts(SERIAL_COM1, " rxdctl ");
	serial_puthex(SERIAL_COM1, r32(REG_RXDCTL0));
	serial_puts(SERIAL_COM1, "\n");
}

static void hw_start(void)
{
	unsigned int ctrl, tctl;

	setup_rings();
	if (pch_lan)
		tx_desc_config();

	/* Make the MAC link state explicit, like e1000's
	 * e1000_setup_copper_link_ich8lan(): the PHY controls speed/duplex,
	 * but SLU must be set and the forced speed/duplex bits cleared. */
	ctrl = r32(REG_CTRL);
	ctrl |= CTRL_SLU;
	ctrl &= ~(CTRL_FRCSPD | CTRL_FRCDPX);
	w32(REG_CTRL, ctrl);

	rx_desc_config();

	w32(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
	/* Preserve the reserved/RRTHRESH bits (82577 reset: bit28=1 and
	 * RRTHRESH=01).  Writing TCTL from scratch cleared them and left the
	 * transmitter unable to fetch descriptors (TDH stuck at 0). */
	tctl = r32(REG_TCTL);
	tctl &= ~(TCTL_CT_MASK | TCTL_COLD_MASK);
	tctl |= TCTL_EN | TCTL_PSP | TCTL_RTLC |
		(TCTL_CT << TCTL_CT_SHIFT) |
		(TCTL_COLD << TCTL_COLD_SHIFT);
	/* The 82577/82578 TCTL reset value has bit 28 = 1 and RRTHRESH
	 * (bits 30:29) = 01.  A warm boot may inherit a firmware value that
	 * cleared them; restore the documented reset state explicitly. */
	if (device_id == 0x10EA || device_id == 0x10EB ||
	    device_id == 0x10EF || device_id == 0x10F0)
		tctl |= 0x30000000u;
	w32(REG_TCTL, tctl);
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

	/* AMT parts: Linux sets CTRL_EXT.DRV_LOAD before the reset that runs
	 * at e1000e_open() time, so the ME knows the driver owns the LAN
	 * while the reset happens. */
	pch_claim();
	log_state("state before reset");

	/* Keep the warm path at probe.  The MAC-only full reset used to
	 * clear inherited RX filter state, but on this PCH it wedges the
	 * machine when it runs during a kexec reload (the board never
	 * reaches the WM loop again).  The firmware filter is overridden by
	 * RCTL.UPE/MPE below, so it is not needed for RX. */
	if (hw_reset(0) != 0) {
		dbg("MAC reset timeout");
		mmio = 0;
		return -1;
	}
	dbg("reset ok");
	pch_claim();
	log_state("state after reset");

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

	/* The 82577 PHY is at MDIC address 1.  Set slow MDIO before reading
	 * the ID, exactly like e1000_init_phy_params_pchlan(); without it the
	 * first ID read often returns 0/0xffff. */
	if (pch_lan) {
		phy_addr = 1;
		pch_slow_mdio();
	}
	phy_probe();
	if (pch_lan) {
		dbg_hex("phy id1", (unsigned int)phy_id1);
		dbg_hex("phy id2", (unsigned int)phy_id2);
		if (phy_addr < 0)
			phy_addr = 1;
		/* phy_probe() may find the PHY at address 2 (X201); set slow
		 * MDIO at the address it actually answered on. */
		pch_slow_mdio();
		pch_phy_workarounds();
	}
	hw_start();
	delay_ms(10);

	/* The full-chip reset drops the link and the PHY has to renegotiate.
	 * Wait for it before net_init runs DHCP, otherwise the first DISCOVER
	 * is transmitted into a dead link. */
	if (pch_lan && !(r32(REG_STATUS) & STATUS_LU)) {
		if (!wait_link(5000))
			dbg("link did not come up after reset");
	}
	{
		unsigned int st = r32(REG_STATUS);

		if (pch_lan && (st & STATUS_LU)) {
			pch_link_up_workarounds();
			/* The PHY reset/link transition can clear these. */
			w32(REG_CTRL, r32(REG_CTRL) | CTRL_SLU);
			w32(REG_RCTL, r32(REG_RCTL) | RCTL_EN);
			w32(REG_TCTL, r32(REG_TCTL) | TCTL_EN);
		}
		dbg(st & STATUS_LU ? "link up" : "link down after reset");
		last_link = !!(st & STATUS_LU);
	}
	log_state("state after start");

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

/* Give a descriptor back to the hardware.  The buffer address is rewritten
 * every time: the extended descriptor write-back overwrites the first 8
 * bytes with RSS/checksum data, so restoring it makes the ring survive even
 * if firmware ever leaves RFCTL.EXTEN set. */
static void rx_rearm(unsigned int entry)
{
	struct e1000e_rx_desc *d = &rx_ring[entry];

	d->addr_lo = (unsigned int)(rx_bufs + entry * E1000E_BUF);
	d->addr_hi = 0;
	d->length = 0;
	d->csum = 0;
	d->status = 0;
	d->errors = 0;
	d->special = 0;
	__sync_synchronize();
	w32(REG_RDT, entry);
}

static int e1000e_rx(unsigned char *buf, unsigned int maxlen)
{
	unsigned int guard = 0;

	if (!present || !mmio)
		return -1;

	link_check();

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
				for (i = 0; i < 6; i++)
					last_rx_dst[i] = buf[i];
				last_rx_len = len;
				if (buf[0] == 0xFF && buf[1] == 0xFF &&
				    buf[2] == 0xFF && buf[3] == 0xFF &&
				    buf[4] == 0xFF && buf[5] == 0xFF)
					rx_bcast++;
				else if (buf[0] & 0x01)
					rx_mcast++;
				else
					rx_ucast++;
				rx_ok++;
				rx_rearm(entry);
				rx_cur = (entry + 1) % E1000E_NRX;
				return (int)len;
			}
			rx_drop++;
		} else {
			rx_err++;
		}

		/* Drop this descriptor and give it back to the hardware. */
		rx_rearm(entry);
		rx_cur = (entry + 1) % E1000E_NRX;
	}
	return -1;
}

/* One-shot snapshot of the transmit state on the first timeout.  On the X201
 * the queue never moved at all, so the interesting values are TDH/TDT,
 * TCTL, TXDCTL and the first descriptor's command word. */
static void tx_log_timeout(void)
{
	if (tx_timeout_logged)
		return;
	tx_timeout_logged = 1;
	dbg("TX timeout");
	dbg_hex("  tctl", r32(REG_TCTL));
	dbg_hex("  tdh", r32(REG_TDH));
	dbg_hex("  tdt", r32(REG_TDT));
	dbg_hex("  txdctl", r32(REG_TXDCTL0));
	dbg_hex("  tdbal", r32(REG_TDBAL));
	dbg_hex("  tdlen", r32(REG_TDLEN));
	dbg_hex("  tarc0", r32(REG_TARC0));
	dbg_hex("  tarc1", r32(REG_TARC1));
	dbg_hex("  tx_cur", tx_cur);
	dbg_hex("  desc_lo", tx_ring[tx_cur].lower);
	dbg_hex("  desc_hi", tx_ring[tx_cur].upper);
}

static int tx_timeout(void)
{
	tx_log_timeout();
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
	link_check();
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
		int lock = pch_swflag_acquire() == 0;

		phy_set_page(0);
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
		if (lock)
			pch_swflag_release();
	} else {
		io_putstr("none");
	}
	io_putstr("txdctl=0x");
	io_puthex(r32(REG_TXDCTL0));
	io_putstr(" tarc=0x");
	io_puthex(r32(REG_TARC0));
	io_putstr(" gcr=0x");
	io_puthex(r32(REG_GCR));
	io_putstr(" rfctl=0x");
	io_puthex(r32(REG_RFCTL));
	io_putstr(" rxcsum=0x");
	io_puthex(r32(REG_RXCSUM));
	io_putstr(" rxdctl=0x");
	io_puthex(r32(REG_RXDCTL0));
	io_putstr("\nextcnf=0x");
	io_puthex(r32(REG_EXTCNF_CTRL));
	io_putstr(" fwsm=0x");
	io_puthex(r32(REG_FWSM));
	io_putstr(" pba=0x");
	io_puthex(r32(REG_PBA));
	io_putstr(" pbs=0x");
	io_puthex(r32(REG_PBS));
	io_putstr(" tdh=0x");
	io_puthex(r32(REG_TDH));
	io_putstr(" tdt=0x");
	io_puthex(r32(REG_TDT));
	io_putchar('\n');
	io_putstr("rx ok ");
	put_dec(rx_ok);
	io_putstr(" uc ");
	put_dec(rx_ucast);
	io_putstr(" bc ");
	put_dec(rx_bcast);
	io_putstr(" mc ");
	put_dec(rx_mcast);
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

static void e1000e_dump_rx(void)
{
	io_putstr("[e1000e rx] ok=");
	put_dec(rx_ok);
	io_putstr(" uc=");
	put_dec(rx_ucast);
	io_putstr(" bc=");
	put_dec(rx_bcast);
	io_putstr(" mc=");
	put_dec(rx_mcast);
	io_putstr(" err=");
	put_dec(rx_err);
	io_putstr(" drop=");
	put_dec(rx_drop);
	io_putstr(" tx=");
	put_dec(tx_ok);
	io_putchar('\n');

	io_putstr("  last status=0x");
	io_puthex(last_rx_status);
	io_putstr(" err=0x");
	io_puthex(last_rx_errors);
	io_putstr(" len=");
	put_dec(last_rx_len);
	io_putstr(" dst=");
	put_mac(last_rx_dst);
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
		{ "PBS", REG_PBS }, { "RCTL", REG_RCTL },
		{ "TCTL", REG_TCTL },
		{ "TIPG", REG_TIPG }, { "RAL0", REG_RAL0 },
		{ "RAH0", REG_RAH0 }, { "RDBAL", REG_RDBAL },
		{ "RDLEN", REG_RDLEN }, { "RDH", REG_RDH },
		{ "RDT", REG_RDT }, { "TDBAL", REG_TDBAL },
		{ "TDLEN", REG_TDLEN }, { "TDH", REG_TDH },
		{ "TDT", REG_TDT }, { "TXDCTL0", REG_TXDCTL0 },
		{ "TXDCTL1", REG_TXDCTL1 }, { "TARC0", REG_TARC0 },
		{ "TARC1", REG_TARC1 }, { "GCR", REG_GCR },
		{ "RXCSUM", REG_RXCSUM }, { "RFCTL", REG_RFCTL },
		{ "RXDCTL0", REG_RXDCTL0 }, { "RXDCTL1", REG_RXDCTL1 },
		{ "EXTCNF", REG_EXTCNF_CTRL }, { "FWSM", REG_FWSM },
		{ "KABGTXD", REG_KABGTXD }, { "CRC_OFF", REG_CRC_OFFSET },
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

	/* Read-clear hardware counters.  CRCERRS/RXERRC/MPC reveal a dirty
	 * 1000M link even when the descriptor error byte is zero; BPRC/BPTC
	 * show whether DHCP/ARP broadcasts are actually moving. */
	{
		unsigned int crcerrs = r32(REG_CRCERRS);
		unsigned int rxerrc = r32(REG_RXERRC);
		unsigned int mpc = r32(REG_MPC);
		unsigned int gprc = r32(REG_GPRC);
		unsigned int bprc = r32(REG_BPRC);
		unsigned int mprc = r32(REG_MPRC);
		unsigned int ruc = r32(REG_RUC);
		unsigned int gptc = r32(REG_GPTC);
		unsigned int bptc = r32(REG_BPTC);

		io_putstr("hw crcerr=");
		put_dec(crcerrs);
		io_putstr(" rxerr=");
		put_dec(rxerrc);
		io_putstr(" mpc=");
		put_dec(mpc);
		io_putstr(" gprc=");
		put_dec(gprc);
		io_putstr(" bprc=");
		put_dec(bprc);
		io_putstr(" mprc=");
		put_dec(mprc);
		io_putstr(" ruc=");
		put_dec(ruc);
		io_putstr(" gptc=");
		put_dec(gptc);
		io_putstr(" bptc=");
		put_dec(bptc);
		io_putchar('\n');
	}

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
	if (rx_ring) {
		io_putstr("rx0 addr=0x");
		io_puthex(rx_ring[0].addr_lo);
		io_putstr(" len=0x");
		io_puthex(rx_ring[0].length);
		io_putstr(" status=0x");
		io_puthex(rx_ring[0].status);
		io_putstr(" errors=0x");
		io_puthex(rx_ring[0].errors);
		io_putstr(" special=0x");
		io_puthex(rx_ring[0].special);
		io_putchar('\n');
	}
}

static void e1000e_reset(void)
{
	if (!present || !mmio || !rx_ring) {
		io_putstr("e1000e: reset: not initialized\n");
		return;
	}
	pch_claim();
	if (hw_reset(0) != 0) {
		io_putstr("e1000e: reset timeout\n");
		return;
	}
	pch_claim();
	read_mac();
	hw_start();
	{
		unsigned int st = r32(REG_STATUS);

		if (pch_lan && (st & STATUS_LU)) {
			pch_link_up_workarounds();
			w32(REG_CTRL, r32(REG_CTRL) | CTRL_SLU);
			w32(REG_RCTL, r32(REG_RCTL) | RCTL_EN);
			w32(REG_TCTL, r32(REG_TCTL) | TCTL_EN);
		}
		last_link = !!(st & STATUS_LU);
		if (!(st & STATUS_LU))
			phy_autoneg();
	}
	tx_ensure_enabled();
	io_putstr("e1000e: netreset done\n");
}

static const unsigned char e1000e_default_ip[4] = { 192, 168, 178, 240 };
static const unsigned char e1000e_default_mask[4] = { 0, 0, 0, 0 };

const struct netdev e1000e_netdev = {
	"e1000e", "Intel e1000e",
	e1000e_init, e1000e_get_mac, e1000e_rx, e1000e_tx, e1000e_print_info,
	e1000e_dump_regs, 0, e1000e_reset, e1000e_set_speed, e1000e_set_mac,
	e1000e_default_ip, e1000e_default_mask, e1000e_dump_rx,
};
