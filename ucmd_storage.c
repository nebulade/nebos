/* storage: ring-3 command.  Refresh the filesystem cache and report devices
 * and mounts (the kernel service re-probes the disks, like the old command). */

#include "ucmd_lib.h"

#define MAX_MOUNTS 24   /* VFS_MAX_MOUNTS */

static struct sys_mount mounts[MAX_MOUNTS];

/* Print a size given in 512-byte sectors (shifts avoid 32-bit overflow). */
static void put_size_sectors(unsigned int sectors)
{
	if (sectors >= 2u * 1024u * 1024u) {           /* >= 1 GiB */
		unsigned int gb = sectors >> 21;
		unsigned int tenths = ((sectors & 0x1FFFFFu) * 10u) >> 21;
		u_putdec(gb);
		if (tenths) {
			u_putc('.');
			u_putdec(tenths);
		}
		u_puts(" GB");
	} else if (sectors >= 2048u) {                 /* >= 1 MiB */
		unsigned int mb = sectors >> 11;
		unsigned int tenths = ((sectors & 0x7FFu) * 10u) >> 11;
		u_putdec(mb);
		if (tenths) {
			u_putc('.');
			u_putdec(tenths);
		}
		u_puts(" MB");
	} else {
		u_putdec(sectors / 2u);
		u_puts(" KB");
	}
}

static void report_fs(const struct sys_mount *m)
{
	u_puts(m->name);
	u_puts(": ");
	u_puts(m->type);
	u_putc(' ');
	put_size_sectors(m->total_sectors);
	if (m->label[0]) {
		u_puts("  \"");
		u_puts(m->label);
		u_putc('"');
	}
	u_putc('\n');
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

void _start(const char *arg)
{
	struct sys_storage_info si;
	unsigned int nmounts = 0, i, j;

	(void)arg;

	/* Refresh the filesystem cache (re-probes all devices). */
	u_syscall1(SYS_STORAGE_INFO, (int)&si);

	for (i = 0; i < MAX_MOUNTS; i++) {
		if (u_syscall2(SYS_LIST_MOUNTS, (int)i, (int)&mounts[nmounts]) != 1)
			break;
		nmounts++;
	}

	for (i = 0; i < si.blkdev_count; i++) {
		struct sys_blkdev d;
		int found = 0;

		if (u_syscall2(SYS_BLKDEV, (int)i, (int)&d) != 1)
			break;

		if (d.kind != 0 /* BLK_FLOPPY */ && !d.present) {
			u_puts(d.name);
			u_puts(": not present\n");
			continue;
		}

		for (j = 0; j < nmounts; j++)
			if (mount_belongs_to(mounts[j].name, d.name)) {
				found = 1;
				break;
			}

		/* Real disks get a header line with model and size, floppies
		 * keep the old single-line output. */
		if (d.kind != 0) {
			u_puts(d.name);
			u_puts(": ");
			if (d.model[0]) {
				u_puts(d.model);
				u_putc(' ');
			}
			put_size_sectors(d.total_sectors);
			if (!found)
				u_puts("  no valid FAT filesystem");
			u_putc('\n');
		} else if (!found) {
			u_puts(d.name);
			u_puts(": no valid FAT filesystem\n");
		}

		if (found)
			for (j = 0; j < nmounts; j++)
				if (mount_belongs_to(mounts[j].name, d.name))
					report_fs(&mounts[j]);
	}

	if (!si.ahci_present)
		u_puts("sata: no AHCI controller\n");
	u_exit(0);
}
