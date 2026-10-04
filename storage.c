/* storage command: refresh the filesystem cache and report devices + mounts. */

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

static void put_size(unsigned int bytes)
{
	if (bytes >= 1024u * 1024u * 1024u) {
		unsigned int gb = bytes / (1024u * 1024u * 1024u);
		unsigned int tenths = (bytes % (1024u * 1024u * 1024u)) * 10u / (1024u * 1024u * 1024u);
		put_dec(gb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" GB");
	} else if (bytes >= 1024u * 1024u) {
		unsigned int mb = bytes / (1024u * 1024u);
		unsigned int tenths = (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u);
		put_dec(mb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" MB");
	} else {
		put_dec(bytes / 1024u);
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
	put_size(fs->total_sectors * fs->bytes_per_sector);
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

		if (d->kind == BLK_IDE && !d->present) {
			io_putstr(d->name);
			io_putstr(": not present\n");
			continue;
		}

		for (j = 0; j < vfs_mount_count; j++) {
			if (mount_belongs_to(vfs_mounts[j].name, d->name)) {
				report_fs(vfs_mounts[j].name, &vfs_mounts[j].fs);
				found = 1;
			}
		}

		if (!found) {
			io_putstr(d->name);
			io_putstr(": no valid FAT filesystem\n");
		}
	}
}
