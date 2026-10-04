/* Virtual filesystem: mount every device with a valid FAT filesystem under `/`. */

#include "vfs.h"
#include "fatdetect.h"
#include "io.h"

vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
unsigned int vfs_mount_count;

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

static void mount_one(blk_dev_t *dev, unsigned int part_lba, const char *name)
{
	fat_fs_t fs;
	unsigned int i;

	if (vfs_mount_count >= VFS_MAX_MOUNTS)
		return;
	if (fat_mount(dev, part_lba, &fs) != 0)
		return;

	for (i = 0; name[i] && i < sizeof(vfs_mounts[vfs_mount_count].name) - 1; i++)
		vfs_mounts[vfs_mount_count].name[i] = name[i];
	vfs_mounts[vfs_mount_count].name[i] = '\0';
	vfs_mounts[vfs_mount_count].fs = fs;
	vfs_mounts[vfs_mount_count].mounted = 1;
	vfs_mount_count++;
}

static void part_name(char *out, const char *base, unsigned int idx)
{
	unsigned int i = 0;
	while (base[i] && i < 13) {
		out[i] = base[i];
		i++;
	}
	out[i++] = 'p';
	out[i++] = (char)('0' + idx + 1);
	out[i] = '\0';
}

void vfs_mount_all(void)
{
	static unsigned char buf[512];
	unsigned int i;

	vfs_mount_count = 0;
	blk_scan();

	for (i = 0; i < blk_dev_count; i++) {
		blk_dev_t *d = &blk_devs[i];

		if (d->kind == BLK_IDE && !d->present)
			continue;

		if (d->kind == BLK_FLOPPY) {
			mount_one(d, 0, d->name);
			continue;
		}

		/* IDE: MBR -> FAT partitions, else superfloppy. */
		{
			mbr_part_t parts[4];
			int n, j;
			int any_fat = 0;

			if (blk_read(d, 0, buf, 1) != 0)
				continue;
			n = mbr_parse(buf, parts);
			for (j = 0; j < n; j++) {
				char pname[16];
				if (!parts[j].is_fat || parts[j].start_lba == 0 ||
				    parts[j].sectors == 0)
					continue;
				any_fat = 1;
				part_name(pname, d->name, parts[j].index);
				mount_one(d, parts[j].start_lba, pname);
			}
			if (!any_fat)
				mount_one(d, 0, d->name);
		}
	}
}

vfs_mount_t *vfs_find(const char *name)
{
	unsigned int i;
	for (i = 0; i < vfs_mount_count; i++) {
		unsigned int j = 0;
		while (vfs_mounts[i].name[j] && name[j] &&
		       vfs_mounts[i].name[j] == name[j])
			j++;
		if (vfs_mounts[i].name[j] == '\0' && name[j] == '\0')
			return &vfs_mounts[i];
	}
	return 0;
}

void vfs_list_root(void)
{
	unsigned int i;
	for (i = 0; i < vfs_mount_count; i++) {
		vfs_mount_t *m = &vfs_mounts[i];
		fat_fs_t *fs = &m->fs;
		io_putstr(m->name);
		io_putstr("  ");
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
}

int vfs_resolve(const char *path, fat_fs_t **fs, const char **fs_path)
{
	unsigned int i = 0, j;
	char name[16];
	vfs_mount_t *m;

	if (!path || path[0] != '/')
		return -1;

	i = 1;
	j = 0;
	while (path[i] && path[i] != '/' && j < sizeof(name) - 1)
		name[j++] = path[i++];
	name[j] = '\0';

	if (j == 0)
		return -1;  /* path is just "/" */

	m = vfs_find(name);
	if (!m)
		return -1;

	*fs = &m->fs;
	*fs_path = path + i;
	return 0;
}
