/* Block device registry: floppy (via FDC) + IDE (via ATA PIO) + SATA (AHCI). */

#include "blkdev.h"
#include "ahci.h"
#include "ata.h"
#include "disk.h"

blk_dev_t blk_devs[BLKDEV_MAX];
unsigned int blk_dev_count;

/* Names for SATA disks, built from the AHCI port number ("sata0".."sata31"). */
static char sata_names[AHCI_MAX_DISKS][8];

static const char *sata_name(unsigned int idx, unsigned int port)
{
	char *b = sata_names[idx];
	unsigned int i = 4;

	b[0] = 's'; b[1] = 'a'; b[2] = 't'; b[3] = 'a';
	if (port >= 10)
		b[i++] = (char)('0' + (port / 10) % 10);
	b[i++] = (char)('0' + port % 10);
	b[i] = '\0';
	return b;
}

static blk_dev_t *add(const char *name, blk_kind_t kind,
		      unsigned int bus, unsigned int unit)
{
	blk_dev_t *d;
	if (blk_dev_count >= BLKDEV_MAX)
		return 0;
	d = &blk_devs[blk_dev_count++];
	d->name = name;
	d->kind = kind;
	d->bus = bus;
	d->unit = unit;
	d->present = 0;
	d->total_sectors = 0;
	d->model[0] = '\0';
	return d;
}

void blk_scan(void)
{
	blk_dev_t *d;

	blk_dev_count = 0;

	/* Floppy drives 0 and 1. */
	d = add("floppy0", BLK_FLOPPY, 0, 0);
	if (d) { d->present = 1; d->total_sectors = 2880; }
	d = add("floppy1", BLK_FLOPPY, 1, 0);
	if (d) { d->present = 1; d->total_sectors = 2880; }

	/* IDE: primary + secondary, master + slave.  The channel port bases
	 * come from the PCI IDE interface (native mode) or the legacy default. */
	ata_init_ports();
	{
		static const char *names[4] = { "ide0", "ide1", "ide2", "ide3" };
		unsigned int i;
		for (i = 0; i < 4; i++) {
			unsigned int sectors = 0;
			char model[41];
			d = add(names[i], BLK_IDE, i / 2, i % 2);
			if (!d)
				continue;
			if (ata_probe(i / 2, i % 2, &sectors, model) == 0) {
				unsigned int j;
				d->present = 1;
				d->total_sectors = sectors;
				for (j = 0; j < 40; j++)
					d->model[j] = model[j];
				d->model[40] = '\0';
			}
		}
	}

	/* SATA/AHCI: only present disks are registered, named by port index. */
	{
		struct ahci_disk disks[AHCI_MAX_DISKS];
		unsigned int n = ahci_scan(disks, AHCI_MAX_DISKS);
		unsigned int i;
		for (i = 0; i < n; i++) {
			unsigned int j;
			d = add(sata_name(i, disks[i].port), BLK_SATA, 0, disks[i].port);
			if (!d)
				break;
			d->present = 1;
			d->total_sectors = disks[i].sectors;
			for (j = 0; j < 40; j++)
				d->model[j] = disks[i].model[j];
			d->model[40] = '\0';
		}
	}
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return (*a == *b);
}

blk_dev_t *blk_find(const char *name)
{
	unsigned int i;
	for (i = 0; i < blk_dev_count; i++)
		if (name_eq(blk_devs[i].name, name))
			return &blk_devs[i];
	return 0;
}

int blk_read(blk_dev_t *d, unsigned int lba, void *buf, unsigned int count)
{
	if (!d || count == 0)
		return -1;
	if (d->kind == BLK_FLOPPY) {
		disk_set_drive(d->bus);
		return disk_read_sectors(lba, buf, count);
	}
	if (d->kind == BLK_SATA)
		return ahci_read_sectors(d->unit, lba, buf, count);
	return ata_read_sectors(d->bus, d->unit, lba, buf, count);
}

int blk_write(blk_dev_t *d, unsigned int lba, const void *buf, unsigned int count)
{
	if (!d || count == 0)
		return -1;
	if (d->kind == BLK_FLOPPY) {
		disk_set_drive(d->bus);
		return disk_write_sectors(lba, buf, count);
	}
	if (d->kind == BLK_SATA)
		return ahci_write_sectors(d->unit, lba, buf, count);
	return ata_write_sectors(d->bus, d->unit, lba, buf, count);
}
