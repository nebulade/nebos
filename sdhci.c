/* Polled SDHCI/eMMC driver for the Intel Braswell MMC controller.
 *
 * The HP Chromebook's eMMC is the SoC MMC function at PCI 00:10.0
 * (8086:2294), a standard SDHCI host controller.  The firmware has already
 * assigned its BAR and powered the card, so the kernel only has to reset the
 * host, run the eMMC identification sequence (CMD0/1/2/3/7/16), read the
 * 512-byte EXT_CSD with CMD8 (which carries SEC_COUNT) and then use CMD17/
 * CMD24 with polled PIO for sector I/O.
 *
 * Everything is polled: there is no IRQ handler and no DMA descriptor ring.
 * The card is kept in its default 1-bit/25 MHz mode; that is slow (a few
 * MB/s) but enough for the storage/format and FAT paths, and avoids the
 * CMD6 bus-width/timing sequencing.  The result is cached after the first
 * probe so the per-command vfs_mount_all() refreshes do not re-initialise
 * the card.
 */
#include "sdhci.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

#define MMC_PCI_VENDOR       0x8086
#define MMC_PCI_DEVICE       0x2294   /* Braswell eMMC */
#define MMC_PCI_DEVICE_SDIO  0x2295
#define MMC_PCI_DEVICE_SD    0x2296

/* SDHCI register offsets. */
#define SDHCI_BLOCK_SIZE      0x04
#define SDHCI_BLOCK_COUNT     0x06
#define SDHCI_ARGUMENT        0x08
#define SDHCI_TRANSFER_MODE   0x0C
#define SDHCI_COMMAND         0x0E
#define SDHCI_RESPONSE        0x10
#define SDHCI_BUFFER          0x20
#define SDHCI_PRESENT_STATE   0x24
#define SDHCI_HOST_CONTROL    0x28
#define SDHCI_POWER_CONTROL   0x29
#define SDHCI_CLOCK_CONTROL   0x2C
#define SDHCI_TIMEOUT_CONTROL 0x2E
#define SDHCI_SOFTWARE_RESET  0x2F
#define SDHCI_INT_STATUS      0x30   /* 32-bit read covers normal + error */
#define SDHCI_INT_STATUS_EN   0x34
#define SDHCI_ERR_STATUS_EN   0x36
#define SDHCI_INT_SIGNAL_EN   0x38
#define SDHCI_ERR_SIGNAL_EN   0x3A
#define SDHCI_HOST_CONTROL2   0x3E
#define SDHCI_CAPABILITIES    0x40
#define SDHCI_HOST_VERSION    0xFE

#define SDHCI_CTRL_VDD_180    0x0008u

/* Present State bits. */
#define SDHCI_CMD_INHIBIT     0x00000001u
#define SDHCI_DAT_INHIBIT     0x00000002u
#define SDHCI_BUF_WRITE       0x00000400u
#define SDHCI_BUF_READ        0x00000800u

/* Normal interrupt bits. */
#define SDHCI_INT_CMD_COMPLETE  0x0001u
#define SDHCI_INT_TRANS_COMPLETE 0x0002u

/* Clock control bits. */
#define SDHCI_CLOCK_INT_EN    0x0001u
#define SDHCI_CLOCK_INT_STABLE 0x0002u
#define SDHCI_CLOCK_CARD_EN   0x0004u

/* Software reset. */
#define SDHCI_RESET_ALL       0x01u

/* Command register flags. */
#define SDHCI_CMD_RESP_NONE   0x00u
#define SDHCI_CMD_RESP_LONG   0x01u
#define SDHCI_CMD_RESP_SHORT  0x02u
#define SDHCI_CMD_RESP_BUSY   0x03u
#define SDHCI_CMD_CRC         0x08u
#define SDHCI_CMD_INDEX       0x10u
#define SDHCI_CMD_DATA        0x20u

/* Transfer mode. */
#define SDHCI_TRNS_READ       0x10u

/* MMC commands used here. */
#define MMC_GO_IDLE_STATE     0
#define MMC_SEND_OP_COND      1
#define MMC_ALL_SEND_CID      2
#define MMC_SET_RELATIVE_ADDR 3
#define MMC_SELECT_CARD       7
#define MMC_SEND_EXT_CSD      8
#define MMC_SWITCH            6
#define MMC_SET_BLOCKLEN      16
#define MMC_READ_SINGLE_BLOCK 17
#define MMC_WRITE_SINGLE_BLOCK 24

#define MMC_OCR_ARG           0x40FF8080u  /* sector mode + full voltage */
#define MMC_RCA               1

#define SDHCI_TIMEOUT_MS      1000
#define SDHCI_DATA_TIMEOUT_MS 5000

static struct {
	int state;                 /* -1 unprobed, 0 absent, 1 ready */
	unsigned int bar;
	unsigned int base_khz;
	unsigned int sectors;
	unsigned int cache_size;
	int high_capacity;
	char model[41];
} mmc = { .state = -1, .model = "" };

static unsigned char rd8(unsigned int off)
{
	return *(volatile unsigned char *)(mmc.bar + off);
}

static unsigned short rd16(unsigned int off)
{
	return *(volatile unsigned short *)(mmc.bar + off);
}

static unsigned int rd32(unsigned int off)
{
	return *(volatile unsigned int *)(mmc.bar + off);
}

static void wr8(unsigned int off, unsigned char v)
{
	*(volatile unsigned char *)(mmc.bar + off) = v;
}

static void wr16(unsigned int off, unsigned short v)
{
	*(volatile unsigned short *)(mmc.bar + off) = v;
}

static void wr32(unsigned int off, unsigned int v)
{
	*(volatile unsigned int *)(mmc.bar + off) = v;
}

static void tiny_delay(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < ms)
		;
}

static int wait_state(unsigned int mask, int set, unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	for (;;) {
		unsigned int v = rd32(SDHCI_PRESENT_STATE);

		if (set ? ((v & mask) == mask) : ((v & mask) == 0))
			return 0;
		if (get_ticks_ms() - t0 > ms)
			return -1;
	}
}

static void log_fail(const char *what, unsigned int cmd, unsigned int sts)
{
	serial_puts(SERIAL_COM1, "sdhci: ");
	serial_puts(SERIAL_COM1, what);
	if (cmd != 0xFFFFFFFFu) {
		serial_puts(SERIAL_COM1, " cmd ");
		serial_putdec(SERIAL_COM1, cmd);
	}
	serial_puts(SERIAL_COM1, " failed, int 0x");
	serial_puthex(SERIAL_COM1, sts);
	serial_puts(SERIAL_COM1, "\n");
}

static int sdhci_pio(void *buf, unsigned int blocks, int write)
{
	unsigned int words = blocks * 128;   /* 512 bytes per block */
	unsigned int i;
	const unsigned int *src = (const unsigned int *)buf;
	unsigned int *dst = (unsigned int *)buf;

	for (i = 0; i < words; i++) {
		unsigned int t0 = get_ticks_ms();

		if (write) {
			while (!(rd32(SDHCI_PRESENT_STATE) & SDHCI_BUF_WRITE)) {
				if (rd32(SDHCI_INT_STATUS) & 0xFFFF0000u)
					return -1;
				if (get_ticks_ms() - t0 > SDHCI_TIMEOUT_MS)
					return -1;
			}
			wr32(SDHCI_BUFFER, *src++);
		} else {
			while (!(rd32(SDHCI_PRESENT_STATE) & SDHCI_BUF_READ)) {
				if (rd32(SDHCI_INT_STATUS) & 0xFFFF0000u)
					return -1;
				if (get_ticks_ms() - t0 > SDHCI_TIMEOUT_MS)
					return -1;
			}
			*dst++ = rd32(SDHCI_BUFFER);
		}
	}
	return 0;
}

static int sdhci_cmd(unsigned int cmd, unsigned int arg, unsigned int flags,
		     unsigned int blocks, unsigned int mode, void *buf)
{
	unsigned int sts = 0;
	unsigned int t0;

	if (wait_state(SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT, 0,
		       SDHCI_TIMEOUT_MS) != 0) {
		log_fail("inhibit", cmd, rd32(SDHCI_PRESENT_STATE));
		return -1;
	}

	wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
	wr32(SDHCI_ARGUMENT, arg);
	if (flags & SDHCI_CMD_DATA) {
		wr16(SDHCI_BLOCK_SIZE, 512);
		wr16(SDHCI_BLOCK_COUNT, (unsigned short)(blocks ? blocks : 1));
		wr8(SDHCI_TRANSFER_MODE, (unsigned char)mode);
	}
	wr16(SDHCI_COMMAND, (unsigned short)((cmd << 8) | flags));

	t0 = get_ticks_ms();
	for (;;) {
		sts = rd32(SDHCI_INT_STATUS);
		if (sts & 0xFFFF0000u) {
			log_fail("command", cmd, sts);
			return -1;
		}
		if (sts & SDHCI_INT_CMD_COMPLETE)
			break;
		if (get_ticks_ms() - t0 > SDHCI_TIMEOUT_MS) {
			log_fail("command timeout", cmd, sts);
			return -1;
		}
	}
	wr32(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

	if (flags & SDHCI_CMD_DATA) {
		if (sdhci_pio(buf, blocks ? blocks : 1,
			      (mode & SDHCI_TRNS_READ) ? 0 : 1) != 0) {
			log_fail("pio", cmd, rd32(SDHCI_INT_STATUS));
			return -1;
		}
		t0 = get_ticks_ms();
		for (;;) {
			sts = rd32(SDHCI_INT_STATUS);
			if (sts & 0xFFFF0000u) {
				log_fail("transfer", cmd, sts);
				return -1;
			}
			if (sts & SDHCI_INT_TRANS_COMPLETE)
				break;
			if (get_ticks_ms() - t0 > SDHCI_DATA_TIMEOUT_MS) {
				log_fail("transfer timeout", cmd, sts);
				return -1;
			}
		}
		wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
	}
	return 0;
}

static int sdhci_clock_div(unsigned int div)
{
	unsigned int t0;

	if (div < 1)
		div = 1;
	if (div > 0xFF)
		div = 0xFF;
	wr16(SDHCI_CLOCK_CONTROL, 0);
	wr16(SDHCI_CLOCK_CONTROL,
	     (unsigned short)((div << 8) | SDHCI_CLOCK_INT_EN));
	t0 = get_ticks_ms();
	while (!(rd16(SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE)) {
		if (get_ticks_ms() - t0 > 100)
			return -1;
	}
	wr16(SDHCI_CLOCK_CONTROL,
	     (unsigned short)((div << 8) | SDHCI_CLOCK_INT_EN |
			      SDHCI_CLOCK_CARD_EN));
	tiny_delay(2);
	return 0;
}

static int sdhci_reset_host(void)
{
	unsigned int t0;

	wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
	t0 = get_ticks_ms();
	while (rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_ALL) {
		if (get_ticks_ms() - t0 > 100)
			return -1;
	}
	return 0;
}

static int pci_find_mmc(unsigned int *bus, unsigned int *dev,
			unsigned int *func)
{
	unsigned int b, d, f;

	for (b = 0; b < 256; b++) {
		for (d = 0; d < 32; d++) {
			unsigned int id = pci_cfg_read32(b, d, 0, 0x00);
			unsigned int htype, nfunc;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			htype = (pci_cfg_read32(b, d, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;
			for (f = 0; f < nfunc; f++) {
				unsigned int idf = pci_cfg_read32(b, d, f, 0x00);
				unsigned int vendor = idf & 0xFFFFu;
				unsigned int device = idf >> 16;
				unsigned int crev, cls, sub;

				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;
				crev = pci_cfg_read32(b, d, f, 0x08);
				cls = (crev >> 24) & 0xFFu;
				sub = (crev >> 16) & 0xFFu;
				if (vendor != MMC_PCI_VENDOR)
					continue;
				if (device == MMC_PCI_DEVICE ||
				    (device != MMC_PCI_DEVICE_SDIO &&
				     device != MMC_PCI_DEVICE_SD &&
				     cls == 0x08 && sub == 0x05)) {
					*bus = b;
					*dev = d;
					*func = f;
					return 0;
				}
			}
		}
	}
	return -1;
}

static int sdhci_init(void)
{
	unsigned int bus, dev, func, idf, cls, sub, bar, cmd, caps;
	unsigned int base_mhz, div, ocr, rca, sec;
	unsigned char ext[512];
	unsigned char power;
	unsigned int i;
	unsigned int t0;
	unsigned short version, hc2;

	mmc.state = 0;
	if (pci_find_mmc(&bus, &dev, &func) != 0) {
		serial_puts(SERIAL_COM1, "sdhci: no Intel MMC controller\n");
		return -1;
	}
	idf = pci_cfg_read32(bus, dev, func, 0x00);
	cls = (pci_cfg_read32(bus, dev, func, 0x08) >> 24) & 0xFFu;
	sub = (pci_cfg_read32(bus, dev, func, 0x08) >> 16) & 0xFFu;
	serial_puts(SERIAL_COM1, "sdhci: PCI ");
	serial_putdec(SERIAL_COM1, bus);
	serial_putc(SERIAL_COM1, ':');
	serial_putdec(SERIAL_COM1, dev);
	serial_putc(SERIAL_COM1, '.');
	serial_putdec(SERIAL_COM1, func);
	serial_puts(SERIAL_COM1, " id ");
	serial_puthex(SERIAL_COM1, idf);
	serial_puts(SERIAL_COM1, " class ");
	serial_puthex(SERIAL_COM1, (cls << 8) | sub);
	serial_puts(SERIAL_COM1, "\n");

	bar = pci_cfg_read32(bus, dev, func, 0x10);
	if ((bar & 1u) || bar == 0 || bar == 0xFFFFFFFFu) {
		serial_puts(SERIAL_COM1, "sdhci: BAR0 is not a usable MMIO address\n");
		return -1;
	}
	if (((bar >> 1) & 3u) == 2u) {
		if (pci_cfg_read32(bus, dev, func, 0x14) != 0) {
			serial_puts(SERIAL_COM1, "sdhci: 64-bit BAR above 4 GB\n");
			return -1;
		}
	}
	bar &= ~0xFu;
	serial_puts(SERIAL_COM1, "sdhci: BAR0 ");
	serial_puthex(SERIAL_COM1, bar);
	serial_puts(SERIAL_COM1, "\n");

	process_map_mmio_all(bar, 0x1000);
	cmd = pci_cfg_read32(bus, dev, func, 0x04);
	pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0006u);
	mmc.bar = bar;
	mmc.state = 0;

	caps = rd32(SDHCI_CAPABILITIES);
	version = rd16(SDHCI_HOST_VERSION);
	base_mhz = (caps >> 8) & 0xFFu;
	if (!base_mhz)
		base_mhz = 50;
	mmc.base_khz = base_mhz * 1000u;
	serial_puts(SERIAL_COM1, "sdhci: caps 0x");
	serial_puthex(SERIAL_COM1, caps);
	serial_puts(SERIAL_COM1, ", version 0x");
	serial_puthex(SERIAL_COM1, version);
	serial_puts(SERIAL_COM1, ", base ");
	serial_putdec(SERIAL_COM1, base_mhz);
	serial_puts(SERIAL_COM1, " MHz");
	if (!((caps >> 8) & 0xFFu))
		serial_puts(SERIAL_COM1, " (default)");
	serial_puts(SERIAL_COM1, "\n");

	if (sdhci_reset_host() != 0) {
		serial_puts(SERIAL_COM1, "sdhci: host reset timeout\n");
		return -1;
	}

	power = 0x0F;   /* bus power + 3.3 V */
	if (!(caps & (1u << 24))) {
		if (caps & (1u << 26))
			power = 0x0B;   /* 1.8 V */
		else if (caps & (1u << 25))
			power = 0x0D;   /* 3.0 V */
	}
	/* The Intel eMMC is often 1.8 V-only (caps bit 26): the power
	 * control selects the rail, and Host Control 2 bit 3 enables 1.8 V
	 * signalling.  Clear the power register first and let the rail
	 * settle before applying the clock. */
	wr8(SDHCI_POWER_CONTROL, 0);
	tiny_delay(10);
	wr8(SDHCI_POWER_CONTROL, power);
	hc2 = rd16(SDHCI_HOST_CONTROL2);
	if (power == 0x0B)
		hc2 |= SDHCI_CTRL_VDD_180;
	else
		hc2 &= (unsigned short)~SDHCI_CTRL_VDD_180;
	wr16(SDHCI_HOST_CONTROL2, hc2);
	tiny_delay(10);
	wr8(SDHCI_TIMEOUT_CONTROL, 0x0E);
	wr8(SDHCI_HOST_CONTROL, 0);
	/* Enable every status bit (we poll the status register) but leave the
	 * interrupt signal disabled.  With the status-enable bits clear the
	 * controller keeps INT_STATUS at zero, which looked like a command
	 * timeout on the first target probe. */
	wr16(SDHCI_INT_STATUS_EN, 0xFFFF);
	wr16(SDHCI_ERR_STATUS_EN, 0xFFFF);
	wr16(SDHCI_INT_SIGNAL_EN, 0);
	wr16(SDHCI_ERR_SIGNAL_EN, 0);
	wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
	if (sdhci_clock_div(0xFF) != 0) {
		serial_puts(SERIAL_COM1, "sdhci: clock did not stabilise\n");
		return -1;
	}
	serial_puts(SERIAL_COM1, "sdhci: power 0x");
	serial_puthex(SERIAL_COM1, power);
	serial_puts(SERIAL_COM1, ", hc2 0x");
	serial_puthex(SERIAL_COM1, hc2);
	serial_puts(SERIAL_COM1, ", clock 0x");
	serial_puthex(SERIAL_COM1, rd16(SDHCI_CLOCK_CONTROL));
	serial_puts(SERIAL_COM1, ", state 0x");
	serial_puthex(SERIAL_COM1, rd32(SDHCI_PRESENT_STATE));
	serial_puts(SERIAL_COM1, "\n");

	if (sdhci_cmd(MMC_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE, 0, 0, 0) != 0)
		return -1;

	t0 = get_ticks_ms();
	ocr = 0;
	for (;;) {
		if (sdhci_cmd(MMC_SEND_OP_COND, MMC_OCR_ARG,
			      SDHCI_CMD_RESP_SHORT, 0, 0, 0) == 0) {
			ocr = rd32(SDHCI_RESPONSE);
			if (ocr & 0x80000000u)
				break;
		}
		if (get_ticks_ms() - t0 > 2000) {
			serial_puts(SERIAL_COM1, "sdhci: CMD1 timeout, ocr 0x");
			serial_puthex(SERIAL_COM1, ocr);
			serial_puts(SERIAL_COM1, "\n");
			return -1;
		}
		tiny_delay(1);
	}
	serial_puts(SERIAL_COM1, "sdhci: ocr 0x");
	serial_puthex(SERIAL_COM1, ocr);
	serial_puts(SERIAL_COM1, "\n");
	mmc.high_capacity = (ocr & 0x40000000u) ? 1 : 0;

	if (sdhci_cmd(MMC_ALL_SEND_CID, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC,
		      0, 0, 0) != 0)
		return -1;
	if (sdhci_cmd(MMC_SET_RELATIVE_ADDR, MMC_RCA << 16,
		      SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
		      0, 0, 0) != 0)
		return -1;
	rca = (rd32(SDHCI_RESPONSE) >> 16) & 0xFFFFu;
	if (!rca)
		rca = MMC_RCA;
	if (sdhci_cmd(MMC_SELECT_CARD, rca << 16,
		      SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
		      0, 0, 0) != 0)
		return -1;
	if (sdhci_cmd(MMC_SET_BLOCKLEN, 512,
		      SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
		      0, 0, 0) != 0)
		return -1;

	for (i = 0; i < sizeof(ext); i++)
		ext[i] = 0;
	if (sdhci_cmd(MMC_SEND_EXT_CSD, 0,
		      SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
			      SDHCI_CMD_DATA,
		      1, SDHCI_TRNS_READ, ext) != 0)
		return -1;

	sec = (unsigned int)ext[212] | ((unsigned int)ext[213] << 8) |
	      ((unsigned int)ext[214] << 16) | ((unsigned int)ext[215] << 24);
	if (sec == 0 || sec == 0xFFFFFFFFu) {
		serial_puts(SERIAL_COM1, "sdhci: EXT_CSD has no sector count\n");
		return -1;
	}
	mmc.cache_size = (unsigned int)ext[249] |
			 ((unsigned int)ext[250] << 8) |
			 ((unsigned int)ext[251] << 16) |
			 ((unsigned int)ext[252] << 24);

	div = mmc.base_khz / 50000u;   /* 25 MHz: base / (2 * 25000) */
	if (!((caps >> 8) & 0xFFu))
		div = 4;               /* conservative if the base is unknown */
	if (sdhci_clock_div(div) != 0) {
		serial_puts(SERIAL_COM1, "sdhci: 25 MHz clock did not stabilise\n");
		return -1;
	}

	mmc.sectors = sec;
	mmc.high_capacity = mmc.high_capacity ? 1 : 0;
	for (i = 0; i < sizeof(mmc.model) - 1; i++)
		mmc.model[i] = 0;
	{
		static const char name[] = "Intel eMMC";
		for (i = 0; name[i] && i < sizeof(mmc.model) - 1; i++)
			mmc.model[i] = name[i];
	}
	mmc.state = 1;

	serial_puts(SERIAL_COM1, "sdhci: eMMC ");
	serial_putdec(SERIAL_COM1, (sec / 2048u) / 1024u);
	serial_puts(SERIAL_COM1, " GiB, ");
	serial_putdec(SERIAL_COM1, sec);
	serial_puts(SERIAL_COM1, " sectors, ");
	serial_puts(SERIAL_COM1, mmc.high_capacity ? "block" : "byte");
	serial_puts(SERIAL_COM1, " addressing\n");
	return 0;
}

unsigned int sdhci_scan(struct sdhci_disk *out, unsigned int max)
{
	unsigned int i;

	if (mmc.state < 0)
		(void)sdhci_init();
	if (mmc.state <= 0 || max == 0)
		return 0;
	out[0].sectors = mmc.sectors;
	for (i = 0; i < sizeof(out[0].model) - 1 && mmc.model[i]; i++)
		out[0].model[i] = mmc.model[i];
	out[0].model[i] = '\0';
	return 1;
}

int sdhci_read_sectors(unsigned int lba, void *buf, unsigned int count)
{
	unsigned int i;

	if (mmc.state <= 0)
		return -1;
	for (i = 0; i < count; i++) {
		unsigned int arg = mmc.high_capacity ? (lba + i) :
				   (lba + i) * 512u;

		if (sdhci_cmd(MMC_READ_SINGLE_BLOCK, arg,
			      SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
				      SDHCI_CMD_INDEX | SDHCI_CMD_DATA,
			      1, SDHCI_TRNS_READ,
			      (unsigned char *)buf + i * 512u) != 0)
			return -1;
	}
	return 0;
}

int sdhci_write_sectors(unsigned int lba, const void *buf, unsigned int count)
{
	unsigned int i;

	if (mmc.state <= 0)
		return -1;
	for (i = 0; i < count; i++) {
		unsigned int arg = mmc.high_capacity ? (lba + i) :
				   (lba + i) * 512u;

		if (sdhci_cmd(MMC_WRITE_SINGLE_BLOCK, arg,
			      SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
				      SDHCI_CMD_INDEX | SDHCI_CMD_DATA,
			      1, 0,
			      (void *)((const unsigned char *)buf + i * 512u)) != 0)
			return -1;
	}
	return 0;
}

int sdhci_flush(void)
{
	/* eMMC CACHE_SIZE (EXT_CSD[249..252]) is non-zero when the card has
	 * a write-back cache.  EXT_CSD_FLUSH_CACHE (index 32) with value 1
	 * commits it; failures are harmless (the card then has no cache). */
	if (mmc.state <= 0 || !mmc.cache_size)
		return 0;
	(void)sdhci_cmd(MMC_SWITCH, 0x0C200100u,
			SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
			0, 0, 0);
	return 0;
}
