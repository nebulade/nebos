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

/* Print a size given in 512-byte sectors.  Uses shifts (not bytes) so a
 * >4 GB filesystem/disk does not overflow 32-bit arithmetic. */
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

		if (d->kind != BLK_FLOPPY && !d->present)
			continue;

		if (d->kind == BLK_FLOPPY) {
			mount_one(d, 0, d->name);
			continue;
		}

		/* IDE/SATA: MBR -> FAT partitions, else superfloppy. */
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
		put_size_sectors(fs->total_sectors);
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

int vfs_join_path(const char *cwd, const char *arg, char *out, unsigned int out_size)
{
	char buf[VFS_PATH_MAX * 2 + 2];
	unsigned int bi = 0;
	unsigned int len;
	const char *p;

	/* Build the combined (un-normalized) path. */
	if (arg[0] == '/') {
		p = arg;
	} else {
		p = cwd;
		while (*p && bi < sizeof(buf) - 1)
			buf[bi++] = *p++;
		if (bi > 0 && buf[bi - 1] != '/') {
			if (bi < sizeof(buf) - 1)
				buf[bi++] = '/';
		}
		p = arg;
	}
	while (*p && bi < sizeof(buf) - 1)
		buf[bi++] = *p++;
	buf[bi] = '\0';

	if (out_size < 2)
		return -1;

	/* Normalize into out (always begins with "/"). */
	out[0] = '/';
	out[1] = '\0';
	len = 1;

	p = buf;
	while (*p) {
		unsigned int clen = 0;
		const char *comp;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		comp = p;
		while (*p && *p != '/') {
			p++;
			clen++;
		}

		if (clen == 1 && comp[0] == '.')
			continue;
		if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
			/* Pop the previous component (may leave "/"). */
			while (len > 1 && out[len - 1] != '/')
				len--;
			if (len > 1)
				len--;
			out[len] = '\0';
			continue;
		}
		/* Append "/component". */
		if (len + 1 + clen + 1 > out_size)
			return -1;
		if (len > 1)
			out[len++] = '/';
		{
			unsigned int k;
			for (k = 0; k < clen; k++)
				out[len++] = comp[k];
		}
		out[len] = '\0';
	}

	return 0;
}

int vfs_device_known(const char *path)
{
	unsigned int i = 0, j = 0;
	char name[16];

	if (!path || path[0] != '/')
		return 0;
	i = 1;
	while (path[i] && path[i] != '/' && j < sizeof(name) - 1)
		name[j++] = path[i++];
	name[j] = '\0';
	if (j == 0)
		return 0;
	return blk_find(name) != 0;
}
