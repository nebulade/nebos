/* ATA PIO driver: IDENTIFY + LBA28/LBA48 read/write, polled (no IRQ).
 * The ATA IRQs (14/15) stay masked at boot, so we poll BSY/DRQ/ERR.
 *
 * Channel port bases default to the legacy primary/secondary ports and are
 * replaced by the PCI IDE interface's BARs when it runs in native mode
 * (nVidia MCP79 "compatibility mode" and friends). */

#include "ata.h"
#include "pci.h"
#include "serial.h"
#include "timer.h"

static unsigned short ata_base[2] = { 0x1F0, 0x170 };
/* Kept for the bring-up log only; transfers poll the command-block status
 * because some controllers' alt-status BAR does not answer correctly. */
static unsigned short ata_ctrl[2] = { 0x3F6, 0x376 };

/* Set when a multi-sector PIO command left DRQ pending on this channel.
 * From then on use one ATA command per sector; slower, but it works on
 * bridges that mishandle multi-sector PIO. */
static int ata_single[2];
static int ata_lba48[2];
static int ata_tf_logged;

/* Register offsets from the I/O base. */
#define REG_DATA    0
#define REG_ERROR   1
#define REG_COUNT   2
#define REG_LBA_LO  3
#define REG_LBA_MID 4
#define REG_LBA_HI  5
#define REG_DRIVE   6
#define REG_CMD     7

/* Status bits (status = base+7, alt-status = ctrl+0). */
#define SR_BSY 0x80
#define SR_DRDY 0x40
#define SR_DRQ 0x08
#define SR_ERR 0x01

/* Commands. */
#define CMD_IDENTIFY  0xEC
#define CMD_READ      0x20   /* LBA28 */
#define CMD_WRITE     0x30   /* LBA28 */
#define CMD_READ_EXT  0x24   /* LBA48 */
#define CMD_WRITE_EXT 0x34   /* LBA48 */

/* A spun-down disk can take several seconds to accept its first command;
 * keep a generous poll budget. */
#define ATA_TIMEOUT_MS 15000

/* Once the data is transferred, the drive should finish the sector quickly.
 * A longer wait here would only delay the single-sector fallback. */
#define ATA_COMPLETE_TIMEOUT_MS 2000

static void ata_reset_channel(unsigned short base, unsigned short ctrl);

/* Point the primary/secondary channels at the PCI IDE interface's native
 * per-channel I/O BARs when it uses native mode, keeping the legacy ports
 * otherwise.  Runs once; without a PCI IDE function the defaults stay. */
void ata_init_ports(void)
{
	static int inited;
	unsigned int bus, dev, func;

	if (inited)
		return;
	inited = 1;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int htype, nfunc;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;
			for (func = 0; func < nfunc; func++) {
				unsigned int idf = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int crev, cls, sub, prog;
				unsigned int bar0, bar1, bar2, bar3, cmd;
				unsigned short b0, c0, b1, c1;

				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;
				crev = pci_cfg_read32(bus, dev, func, 0x08);
				cls  = (crev >> 24) & 0xFFu;
				sub  = (crev >> 16) & 0xFFu;
				prog = (crev >> 8) & 0xFFu;
				if (cls != 0x01 || sub != 0x01)
					continue;

				cmd = pci_cfg_read32(bus, dev, func, 0x04);
				pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0005u);
				bar0 = pci_cfg_read32(bus, dev, func, 0x10);
				bar1 = pci_cfg_read32(bus, dev, func, 0x14);
				bar2 = pci_cfg_read32(bus, dev, func, 0x18);
				bar3 = pci_cfg_read32(bus, dev, func, 0x1C);

				/* prog-if bit 0/2: primary/secondary native mode. */
				if ((prog & 0x01u) && (bar0 & 0x1u)) {
					b0 = (unsigned short)(bar0 & 0xFFFCu);
					/* A native-mode BAR1 of 0 is malformed;
					 * the legacy control port is a safer
					 * fallback than b0+8. */
					c0 = (bar1 & 0x1u)
						? (unsigned short)(bar1 & 0xFFFCu)
						: (unsigned short)0x3F6;
				} else {
					b0 = 0x1F0; c0 = 0x3F6;
				}
				if ((prog & 0x04u) && (bar2 & 0x1u)) {
					b1 = (unsigned short)(bar2 & 0xFFFCu);
					c1 = (bar3 & 0x1u)
						? (unsigned short)(bar3 & 0xFFFCu)
						: (unsigned short)0x376;
				} else {
					b1 = 0x170; c1 = 0x376;
				}

				ata_base[0] = b0; ata_ctrl[0] = c0;
				ata_base[1] = b1; ata_ctrl[1] = c1;
				ata_reset_channel(ata_base[0], ata_ctrl[0]);
				ata_reset_channel(ata_base[1], ata_ctrl[1]);

				serial_puts(SERIAL_COM1, "ata: ctl ");
				serial_puthex(SERIAL_COM1, port_inb(c0));
				serial_putc(SERIAL_COM1, '/');
				serial_puthex(SERIAL_COM1, port_inb(c0 + 2));
				serial_putc(SERIAL_COM1, ' ');
				serial_puthex(SERIAL_COM1, port_inb(c1));
				serial_putc(SERIAL_COM1, '/');
				serial_puthex(SERIAL_COM1, port_inb(c1 + 2));
				serial_putc(SERIAL_COM1, '\n');

				serial_puts(SERIAL_COM1, "ata: pci ide ");
				serial_putdec(SERIAL_COM1, bus);
				serial_putc(SERIAL_COM1, ':');
				serial_putdec(SERIAL_COM1, dev);
				serial_putc(SERIAL_COM1, '.');
				serial_putdec(SERIAL_COM1, func);
				serial_puts(SERIAL_COM1, " prog ");
				serial_puthex(SERIAL_COM1, prog);
				serial_puts(SERIAL_COM1, " ch0 ");
				serial_puthex(SERIAL_COM1, b0);
				serial_putc(SERIAL_COM1, '-');
				serial_puthex(SERIAL_COM1, c0);
				serial_puts(SERIAL_COM1, " ch1 ");
				serial_puthex(SERIAL_COM1, b1);
				serial_putc(SERIAL_COM1, '-');
				serial_puthex(SERIAL_COM1, c1);
				serial_putc(SERIAL_COM1, '\n');
				return;
			}
		}
	}
	serial_puts(SERIAL_COM1, "ata: no PCI IDE function, legacy ports\n");
}

/* Delay 400 ns by reading the command-block status register a few times.
 * The alt-status port is not used for polling: some IDE functions expose a
 * BAR that does not answer correctly, and a bogus BSY bit there made every
 * transfer time out even though the command block reported ready. */
static void ata_400ns(unsigned short base)
{
	(void)port_inb(base + REG_CMD);
	(void)port_inb(base + REG_CMD);
	(void)port_inb(base + REG_CMD);
	(void)port_inb(base + REG_CMD);
}

static int ata_wait_not_busy(unsigned short base, unsigned int timeout_ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < timeout_ms) {
		if (!(port_inb(base + REG_CMD) & SR_BSY))
			return 0;
	}
	return -1;
}

/* Assert SRST for ~10 ms.  This aborts a command a crashed/reloaded kernel
 * left half-transferred (the drive would otherwise stay in DRQ forever and
 * ignore the next IDENTIFY).  Best effort: a malformed control BAR simply
 * makes the reset a no-op. */
static void ata_reset_channel(unsigned short base, unsigned short ctrl)
{
	unsigned int t0;

	port_outb(ctrl, 0x04);                    /* SRST */
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 10)
		;
	/* nIEN=1: the driver polls, so keep the drive's IRQ line quiet
	 * instead of leaving a masked level-triggered interrupt asserted. */
	port_outb(ctrl, 0x02);
	t0 = get_ticks_ms();
	while ((port_inb(base + REG_CMD) & SR_BSY) &&
	       get_ticks_ms() - t0 < 1000)
		;
	ata_400ns(base);
}

static int ata_wait_drq(unsigned short base, unsigned int timeout_ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < timeout_ms) {
		unsigned char st = port_inb(base + REG_CMD);
		if (st & SR_ERR)
			return -1;
		if (st & SR_DRQ)
			return 0;
	}
	return -1;
}

/* A command is done only when both BSY and DRQ are clear.  Waiting for BSY
 * alone can return while the last sector is still pending; the next command
 * is then ignored and the drive stays in DRQ, which looks like a zeroed /
 * unmountable filesystem. */
static int ata_wait_idle(unsigned short base, unsigned int timeout_ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < timeout_ms) {
		unsigned char st = port_inb(base + REG_CMD);

		if (st & SR_ERR)
			return -1;
		if (!(st & (SR_BSY | SR_DRQ)))
			return 0;
	}
	return -1;
}

static int ata_identify(int bus, int unit, unsigned short *id)
{
	unsigned short base = ata_base[bus];
	unsigned char st;
	unsigned int t0;
	unsigned int i;

	/* A previous failed transfer can leave the drive waiting for PIO
	 * data, in which case it ignores IDENTIFY and returns zeros. */
	if (ata_wait_idle(base, 1000) != 0)
		ata_reset_channel(base, ata_ctrl[bus]);

	port_outb(base + REG_DRIVE, (unsigned char)(0xA0 | (unit << 4)));
	port_outb(base + REG_COUNT, 0);
	port_outb(base + REG_LBA_LO, 0);
	port_outb(base + REG_LBA_MID, 0);
	port_outb(base + REG_LBA_HI, 0);
	port_outb(base + REG_CMD, CMD_IDENTIFY);

	ata_400ns(base);

	t0 = get_ticks_ms();
	for (;;) {
		st = port_inb(base + REG_CMD);
		if (st == 0 || st == 0xFF)   /* floating bus: no device */
			return -1;
		if (!(st & SR_BSY))
			break;
		if (get_ticks_ms() - t0 > ATA_TIMEOUT_MS)
			return -1;
	}
	if (!(st & SR_DRQ))
		return -1;

	for (i = 0; i < 256; i++)
		id[i] = port_inw(base + REG_DATA);
	return 0;
}

int ata_identify_parse(const unsigned short *id, unsigned int *total_sectors, char model[41])
{
	unsigned int sectors, lo, hi, i;

	if (id[0] & 0x8000)     /* ATAPI (cdrom/zip): not a data disk */
		return -1;

	/* Model string: words 27..46, byte-swapped. */
	for (i = 0; i < 20; i++) {
		unsigned short w = id[27 + i];
		model[i * 2]     = (char)(w >> 8);
		model[i * 2 + 1] = (char)(w & 0xFF);
	}
	model[40] = '\0';
	for (i = 40; i > 0; i--) {
		if (model[i - 1] == ' ')
			model[i - 1] = '\0';
		else
			break;
	}

	sectors = (unsigned int)id[60] | ((unsigned int)id[61] << 16);
	if (id[83] & 0x0400) {  /* LBA48 supported */
		lo = (unsigned int)id[100] | ((unsigned int)id[101] << 16);
		hi = (unsigned int)id[102] | ((unsigned int)id[103] << 16);
		if (lo || hi)
			sectors = lo;
		if (hi)
			sectors = 0xFFFFFFFFu;  /* > 2 TB: clamp to 32-bit LBA */
	}
	*total_sectors = sectors;
	return 0;
}

int ata_probe(int bus, int unit, unsigned int *total_sectors, char model[41])
{
	static unsigned short id[256];
	int r;

	if (ata_identify(bus, unit, id) != 0)
		return -1;

	r = ata_identify_parse(id, total_sectors, model);
	if (r == 0)
		ata_lba48[bus] = (id[83] & 0x0400) != 0;
	return r;
}

static void ata_log_failure(int write, unsigned int lba, unsigned int count,
			    const char *what, unsigned short base)
{
	unsigned char st = port_inb(base + REG_CMD);
	unsigned char err = port_inb(base + REG_ERROR);

	serial_puts(SERIAL_COM1, "ata: ");
	serial_puts(SERIAL_COM1, write ? "write" : "read");
	serial_puts(SERIAL_COM1, " failed (");
	serial_puts(SERIAL_COM1, what);
	serial_puts(SERIAL_COM1, ") lba ");
	serial_putdec(SERIAL_COM1, lba);
	serial_puts(SERIAL_COM1, " count ");
	serial_putdec(SERIAL_COM1, count);
	serial_puts(SERIAL_COM1, " status ");
	serial_puthex(SERIAL_COM1, st);
	serial_puts(SERIAL_COM1, " error ");
	serial_puthex(SERIAL_COM1, err);
	serial_puts(SERIAL_COM1, " cnt ");
	serial_puthex(SERIAL_COM1, port_inb(base + REG_COUNT));
	serial_puts(SERIAL_COM1, " lba ");
	serial_puthex(SERIAL_COM1,
		      port_inb(base + REG_LBA_LO) |
		      (port_inb(base + REG_LBA_MID) << 8) |
		      (port_inb(base + REG_LBA_HI) << 16));
	serial_putc(SERIAL_COM1, '\n');
}

/* Use explicit 16-bit port reads/writes instead of `rep ins/outs`.  The
 * string form writes through ES:EDI; the C loop uses the normal data
 * segment and is the same path IDENTIFY already uses successfully on the
 * real MCP79 board.  A sector is only 256 words, so the speed difference
 * is small compared with the drive's PIO timing. */
static void ata_insw(unsigned short port, void *dst, unsigned int count)
{
	unsigned short *p = (unsigned short *)dst;

	while (count--)
		*p++ = port_inw(port);
}

static void ata_outsw(unsigned short port, const void *src, unsigned int count)
{
	const unsigned short *p = (const unsigned short *)src;

	while (count--)
		port_outw(port, *p++);
}

static int ata_xfer(int bus, int unit, unsigned int lba, void *buf,
		    unsigned int count, int write)
{
	unsigned short base = ata_base[bus];
	unsigned char *p = buf;
	unsigned int done = 0, i;

	if (ata_wait_idle(base, 1000) != 0)
		ata_reset_channel(base, ata_ctrl[bus]);

	while (done < count) {
		unsigned int chunk = count - done;
		int lba48;
		unsigned int cur = lba + done;

		if (ata_single[bus])
			chunk = 1;
		else if (chunk > 255)
			chunk = 255;
		lba48 = ata_lba48[bus] ||
			((cur + chunk - 1) >= 0x10000000u);

		{
			unsigned char cmd;

			if (lba48) {
				port_outb(base + REG_DRIVE, (unsigned char)(0x40 | (unit << 4)));
				port_outb(base + REG_COUNT, (unsigned char)(chunk >> 8));
				port_outb(base + REG_LBA_LO, (unsigned char)(cur >> 24));
				port_outb(base + REG_LBA_MID, 0);
				port_outb(base + REG_LBA_HI, 0);
				port_outb(base + REG_COUNT, (unsigned char)(chunk & 0xFF));
				port_outb(base + REG_LBA_LO, (unsigned char)(cur & 0xFF));
				port_outb(base + REG_LBA_MID, (unsigned char)((cur >> 8) & 0xFF));
				port_outb(base + REG_LBA_HI, (unsigned char)((cur >> 16) & 0xFF));
				cmd = (unsigned char)(write ? CMD_WRITE_EXT : CMD_READ_EXT);
			} else {
				port_outb(base + REG_DRIVE,
					  (unsigned char)(0xE0 | (unit << 4) | ((cur >> 24) & 0x0F)));
				port_outb(base + REG_COUNT, (unsigned char)(chunk & 0xFF));
				port_outb(base + REG_LBA_LO, (unsigned char)(cur & 0xFF));
				port_outb(base + REG_LBA_MID, (unsigned char)((cur >> 8) & 0xFF));
				port_outb(base + REG_LBA_HI, (unsigned char)((cur >> 16) & 0xFF));
				cmd = (unsigned char)(write ? CMD_WRITE : CMD_READ);
			}

			if (!ata_tf_logged) {
				ata_tf_logged = 1;
				serial_puts(SERIAL_COM1, "ata: tf cnt ");
				serial_puthex(SERIAL_COM1, port_inb(base + REG_COUNT));
				serial_puts(SERIAL_COM1, " lba ");
				serial_puthex(SERIAL_COM1,
					      port_inb(base + REG_LBA_LO) |
					      (port_inb(base + REG_LBA_MID) << 8) |
					      (port_inb(base + REG_LBA_HI) << 16));
				serial_puts(SERIAL_COM1, " drv ");
				serial_puthex(SERIAL_COM1, port_inb(base + REG_DRIVE));
				serial_puts(SERIAL_COM1, " cmd ");
				serial_puthex(SERIAL_COM1, cmd);
				serial_putc(SERIAL_COM1, '\n');
			}
			port_outb(base + REG_CMD, cmd);
		}

		ata_400ns(base);

		/* PIO transfers must wait for DRQ before *each* sector: the
		 * drive drops DRQ between sectors while it commits the previous
		 * one, and writing into the data register while DRQ is low is
		 * silently lost.  A single rep outsw over a large chunk looked
		 * fine for one sector but left the drive waiting for more data
		 * (DRQ set, count/LBA registers advanced), so the next command
		 * read stale data and a large format eventually failed. */
		for (i = 0; i < chunk; i++) {
			if (ata_wait_not_busy(base, ATA_TIMEOUT_MS) != 0) {
				ata_log_failure(write, cur + i, 1,
						"busy timeout", base);
				goto xfer_fail;
			}
			if (ata_wait_drq(base, ATA_TIMEOUT_MS) != 0) {
				ata_log_failure(write, cur + i, 1,
						"drq timeout", base);
				goto xfer_fail;
			}
			if (write)
				ata_outsw(base + REG_DATA, p + i * 512, 256);
			else
				ata_insw(base + REG_DATA, p + i * 512, 256);
		}

		/* Let the last sector finish before the next command. */
		if (ata_wait_idle(base, ATA_COMPLETE_TIMEOUT_MS) != 0) {
			ata_log_failure(write, cur, chunk, "completion timeout", base);
			goto xfer_fail;
		}
		if (port_inb(base + REG_CMD) & SR_ERR) {
			ata_log_failure(write, cur, chunk, "transfer error", base);
			goto xfer_fail;
		}

		p += chunk * 512;
		done += chunk;
		continue;

xfer_fail:
		ata_reset_channel(base, ata_ctrl[bus]);
		if (ata_single[bus])
			return -1;
		/* A multi-sector command upset this bridge; retry from the same
		 * sector with one ATA command per sector. */
		ata_single[bus] = 1;
		serial_puts(SERIAL_COM1, "ata: switching to single-sector PIO\n");
	}
	return 0;
}

int ata_read_sectors(int bus, int unit, unsigned int lba, void *buf, unsigned int count)
{
	return ata_xfer(bus, unit, lba, buf, count, 0);
}

int ata_write_sectors(int bus, int unit, unsigned int lba, const void *buf, unsigned int count)
{
	return ata_xfer(bus, unit, lba, (void *)buf, count, 1);
}

int ata_flush(int bus, int unit)
{
	unsigned short base;

	if (bus < 0 || bus > 1)
		return -1;
	base = ata_base[bus];
	if (ata_wait_idle(base, 1000) != 0)
		ata_reset_channel(base, ata_ctrl[bus]);
	port_outb(base + REG_DRIVE, (unsigned char)(0xA0 | (unit << 4)));
	port_outb(base + REG_CMD, 0xE7);          /* FLUSH CACHE */
	ata_400ns(base);
	if (ata_wait_idle(base, ATA_TIMEOUT_MS) != 0) {
		ata_log_failure(1, 0, 0, "cache flush timeout", base);
		ata_reset_channel(base, ata_ctrl[bus]);
		return -1;
	}
	if (port_inb(base + REG_CMD) & SR_ERR) {
		ata_log_failure(1, 0, 0, "cache flush error", base);
		ata_reset_channel(base, ata_ctrl[bus]);
		return -1;
	}
	return 0;
}
