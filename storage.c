/* storage command: refresh the filesystem cache and report devices + mounts. */

#include "ahci.h"
#include "blkdev.h"
#include "io.h"
#include "vfs.h"

static void put_dec(unsigned int n)
{
	char buf[12];
	unsigned int i = sizeof(buf) - 1;
	buf[i] = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i > 0) {
		buf[--i] = (char)('0' + (n % 10));
		n /= 10;
	}
	io_putstr(buf + i);
}

/* Print a size given in 512-byte sectors (shifts avoid 32-bit overflow). */
static void put_size_sectors(unsigned int sectors)
{
	if (sectors >= 2u * 1024u * 1024u) {           /* >= 1 GiB */
		unsigned int gb = sectors >> 21;
		unsigned int tenths = ((sectors & 0x1FFFFFu) * 10u) >> 21;
		put_dec(gb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" GB");
	} else if (sectors >= 2048u) {                 /* >= 1 MiB */
		unsigned int mb = sectors >> 11;
		unsigned int tenths = ((sectors & 0x7FFu) * 10u) >> 11;
		put_dec(mb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" MB");
	} else {
		put_dec(sectors / 2u);
		io_putstr(" KB");
	}
}

static const char *fat_name(fat_type_t t)
{
	switch (t) {
	case FAT12: return "FAT12";
	case FAT16: return "FAT16";
	case FAT32: return "FAT32";
	default:    return "FAT";
	}
}

static void report_fs(const char *name, const fat_fs_t *fs)
{
	io_putstr(name);
	io_putstr(": ");
	io_putstr(fat_name(fs->type));
	io_putchar(' ');
	put_size_sectors(fs->total_sectors);
	if (fs->label[0]) {
		io_putstr("  \"");
		io_putstr(fs->label);
		io_putchar('"');
	}
	io_putchar('\n');
}

/* Does mount `mount_name` belong to block device `dev_name`?
 * True for an exact match ("floppy0") or a partition ("ide0p1" for "ide0"). */
static int mount_belongs_to(const char *mount_name, const char *dev_name)
{
	unsigned int i = 0;
	while (dev_name[i] && mount_name[i] == dev_name[i])
		i++;
	if (dev_name[i] == '\0' && mount_name[i] == '\0')
		return 1;
	if (dev_name[i] == '\0' && mount_name[i] == 'p')
		return 1;
	return 0;
}

void program_storage_main(void)
{
	unsigned int i, j;

	/* Refresh the filesystem cache (re-probes all devices). */
	vfs_mount_all();

	for (i = 0; i < blk_dev_count; i++) {
		blk_dev_t *d = &blk_devs[i];
		int found = 0;

		if (d->kind != BLK_FLOPPY && !d->present) {
			io_putstr(d->name);
			io_putstr(": not present\n");
			continue;
		}

		for (j = 0; j < vfs_mount_count; j++)
			if (mount_belongs_to(vfs_mounts[j].name, d->name)) {
				found = 1;
				break;
			}

		/* Real disks get a header line with model and size, floppies
		 * keep the old single-line output. */
		if (d->kind != BLK_FLOPPY) {
			io_putstr(d->name);
			io_putstr(": ");
			if (d->model[0]) {
				io_putstr(d->model);
				io_putchar(' ');
			}
			put_size_sectors(d->total_sectors);
			if (!found)
				io_putstr("  no valid FAT filesystem");
			io_putchar('\n');
		} else if (!found) {
			io_putstr(d->name);
			io_putstr(": no valid FAT filesystem\n");
		}

		if (found)
			for (j = 0; j < vfs_mount_count; j++)
				if (mount_belongs_to(vfs_mounts[j].name, d->name))
					report_fs(vfs_mounts[j].name, &vfs_mounts[j].fs);
	}

	if (!ahci_controller_present())
		io_putstr("sata: no AHCI controller\n");
}
