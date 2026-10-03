/* ATA PIO driver: IDENTIFY + LBA28/LBA48 read/write, polled (no IRQ).
 * The ATA IRQs (14/15) stay masked at boot, so we poll BSY/DRQ/ERR. */

#include "ata.h"
#include "pci.h"
#include "timer.h"

static const unsigned short ata_base[2] = { 0x1F0, 0x170 };
static const unsigned short ata_ctrl[2] = { 0x3F6, 0x376 };

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

/* Delay 400 ns by reading the alt-status register a few times. */
static void ata_400ns(unsigned short ctrl)
{
	(void)port_inb(ctrl);
	(void)port_inb(ctrl);
	(void)port_inb(ctrl);
	(void)port_inb(ctrl);
}

static int ata_wait_not_busy(unsigned short ctrl, unsigned int timeout_ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < timeout_ms) {
		if (!(port_inb(ctrl) & SR_BSY))
			return 0;
	}
	return -1;
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

static int ata_identify(int bus, int unit, unsigned short *id)
{
	unsigned short base = ata_base[bus];
	unsigned short ctrl = ata_ctrl[bus];
	unsigned char st;
	unsigned int t0;
	unsigned int i;

	port_outb(base + REG_DRIVE, (unsigned char)(0xA0 | (unit << 4)));
	port_outb(base + REG_COUNT, 0);
	port_outb(base + REG_LBA_LO, 0);
	port_outb(base + REG_LBA_MID, 0);
	port_outb(base + REG_LBA_HI, 0);
	port_outb(base + REG_CMD, CMD_IDENTIFY);

	ata_400ns(ctrl);

	t0 = get_ticks_ms();
	for (;;) {
		st = port_inb(base + REG_CMD);
		if (st == 0 || st == 0xFF)   /* floating bus: no device */
			return -1;
		if (!(st & SR_BSY))
			break;
		if (get_ticks_ms() - t0 > 4000)
			return -1;
	}
	if (!(st & SR_DRQ))
		return -1;

	for (i = 0; i < 256; i++)
		id[i] = port_inw(base + REG_DATA);
	return 0;
}

int ata_probe(int bus, int unit, unsigned int *total_sectors, char model[41])
{
	static unsigned short id[256];
	unsigned int sectors, lo, hi, i;

	if (ata_identify(bus, unit, id) != 0)
		return -1;

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
	}
	*total_sectors = sectors;
	return 0;
}

static inline void ata_insw(unsigned short port, void *dst, unsigned int count)
{
	__asm__ volatile ("rep insw"
		: "+D"(dst), "+c"(count)
		: "d"(port)
		: "memory");
}

static inline void ata_outsw(unsigned short port, const void *src, unsigned int count)
{
	__asm__ volatile ("rep outsw"
		: "+S"(src), "+c"(count)
		: "d"(port)
		: "memory");
}

static int ata_xfer(int bus, int unit, unsigned int lba, void *buf,
		    unsigned int count, int write)
{
	unsigned short base = ata_base[bus];
	unsigned short ctrl = ata_ctrl[bus];
	unsigned char *p = buf;
	unsigned int done = 0;

	while (done < count) {
		unsigned int chunk = count - done;
		int lba48;
		unsigned int cur = lba + done;

		if (chunk > 255)
			chunk = 255;
		lba48 = ((cur + chunk - 1) >= 0x10000000u);

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
			port_outb(base + REG_CMD, (unsigned char)(write ? CMD_WRITE_EXT : CMD_READ_EXT));
		} else {
			port_outb(base + REG_DRIVE,
				  (unsigned char)(0xE0 | (unit << 4) | ((cur >> 24) & 0x0F)));
			port_outb(base + REG_COUNT, (unsigned char)(chunk & 0xFF));
			port_outb(base + REG_LBA_LO, (unsigned char)(cur & 0xFF));
			port_outb(base + REG_LBA_MID, (unsigned char)((cur >> 8) & 0xFF));
			port_outb(base + REG_LBA_HI, (unsigned char)((cur >> 16) & 0xFF));
			port_outb(base + REG_CMD, (unsigned char)(write ? CMD_WRITE : CMD_READ));
		}

		ata_400ns(ctrl);
		if (ata_wait_not_busy(ctrl, 4000) != 0)
			return -1;
		if (ata_wait_drq(base, 4000) != 0)
			return -1;

		if (write) {
			ata_outsw(base + REG_DATA, p, chunk * 256);
			/* Wait for the drive to finish writing to media (BSY clears
			 * after the data transfer) before issuing the next command. */
			if (ata_wait_not_busy(ctrl, 4000) != 0)
				return -1;
			if (port_inb(base + REG_CMD) & SR_ERR)
				return -1;
		} else {
			ata_insw(base + REG_DATA, p, chunk * 256);
		}

		p += chunk * 512;
		done += chunk;
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
