/* Virtual filesystem: mount every device with a valid FAT filesystem under `/`,
 * and the command pack (ramfs) at /ramfs.  File operations go through the
 * fs_ops vtable so sys_fs.c and the shell stay filesystem-agnostic. */

#include "vfs.h"
#include "fatdetect.h"
#include "io.h"
#include "ramfs.h"

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

static void mem_copy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	while (n--)
		*d++ = *s++;
}

static void copy_bounded(char *dst, const char *src, unsigned int size)
{
	unsigned int i = 0;

	while (i + 1 < size && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

/* ---- FAT implementation ---- */

/* One load buffer for the shell: only one command is loaded at a time and the
 * ELF loader has copied the segments out before we return. */
static unsigned char fat_load_buf[VFS_FILE_MAX];

static int fat_op_read_file(struct vfs_mount *m, const char *path,
			    void *buf, unsigned int max)
{
	unsigned int dir;
	char name[12];
	int r = fat_resolve_path(&m->fs, path, &dir, name);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_ENOENT;
	r = fat_read_file_in(&m->fs, dir, name, buf, max);
	if (r == -2)
		return VFS_EIO;
	if (r < 0)
		return VFS_ENOENT;
	return r;
}

static int fat_op_stat(struct vfs_mount *m, const char *path,
		       unsigned int *size, int *is_dir)
{
	int r = fat_stat(&m->fs, path, size, is_dir);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_ENOENT;
	return 0;
}

static int fat_op_load_file(struct vfs_mount *m, const char *path,
			    struct vfs_file *out)
{
	unsigned int size = 0;
	int is_dir = 0, r;

	r = fat_op_stat(m, path, &size, &is_dir);
	if (r != 0)
		return r;
	if (is_dir || size == 0 || size > sizeof(fat_load_buf))
		return VFS_EINVAL;
	r = fat_op_read_file(m, path, fat_load_buf, sizeof(fat_load_buf));
	if (r < 0)
		return r;
	out->data = fat_load_buf;
	out->size = (unsigned int)r;
	return 0;
}

static void fat_op_release_file(struct vfs_mount *m, struct vfs_file *f)
{
	(void)m;
	(void)f;
}

static int fat_op_write_file(struct vfs_mount *m, const char *path,
			     const void *buf, unsigned int len)
{
	unsigned int dir;
	char name[12];
	int r = fat_resolve_path(&m->fs, path, &dir, name);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;
	r = fat_write_file_in(&m->fs, dir, name, buf, len);
	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;
	return 0;
}

static int fat_op_unlink(struct vfs_mount *m, const char *path)
{
	unsigned int dir;
	char name[12];
	int r = fat_resolve_path(&m->fs, path, &dir, name);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_ENOENT;
	r = fat_delete_file_in(&m->fs, dir, name);
	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_ENOENT;
	return 0;
}

static int fat_op_mkdir(struct vfs_mount *m, const char *path)
{
	unsigned int dir;
	char name[12];
	int r = fat_resolve_path(&m->fs, path, &dir, name);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;
	r = fat_mkdir(&m->fs, dir, name);
	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;   /* the FAT layer printed its own reason */
	return 0;
}

static int fat_op_rmdir(struct vfs_mount *m, const char *path)
{
	unsigned int dir;
	char name[12];
	int r = fat_resolve_path(&m->fs, path, &dir, name);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;
	r = fat_rmdir(&m->fs, dir, name);
	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_EINVAL;
	return 0;
}

/* Index-based listing: fat_list_dir walks from the start each call and we
 * count past `index` entries.  Directories are small. */
static unsigned int fat_list_index;
static struct vfs_dirent *fat_list_out;
static int fat_list_done;
static unsigned int fat_list_seen;

static void fat_list_cb(const char *name, int is_dir, unsigned int size)
{
	unsigned int i;

	if (fat_list_done || fat_list_seen < fat_list_index) {
		fat_list_seen++;
		return;
	}
	for (i = 0; i < 11; i++)
		fat_list_out->name[i] = name[i] ? name[i] : ' ';
	fat_list_out->name[11] = '\0';
	fat_list_out->size = size;
	fat_list_out->is_dir = (unsigned int)is_dir;
	fat_list_done = 1;
	fat_list_seen++;
}

static int fat_op_list_dir(struct vfs_mount *m, const char *path,
			   unsigned int index, struct vfs_dirent *out)
{
	unsigned int dir;
	int r = fat_resolve_dir(&m->fs, path, &dir);

	if (r == -2)
		return VFS_EIO;
	if (r != 0)
		return VFS_ENOENT;   /* not found (or not a directory) */

	fat_list_index = index;
	fat_list_seen = 0;
	fat_list_out = out;
	fat_list_done = 0;
	r = fat_list_dir(&m->fs, dir, fat_list_cb);
	if (r == -2)
		return VFS_EIO;
	return fat_list_done ? 1 : 0;
}

static const struct fs_ops fat_ops = {
	.raw_83 = 1,
	.read_file = fat_op_read_file,
	.load_file = fat_op_load_file,
	.release_file = fat_op_release_file,
	.stat = fat_op_stat,
	.write_file = fat_op_write_file,
	.unlink = fat_op_unlink,
	.mkdir = fat_op_mkdir,
	.rmdir = fat_op_rmdir,
	.list_dir = fat_op_list_dir,
};

/* ---- ramfs implementation ---- */

static int ramfs_op_read_file(struct vfs_mount *m, const char *path,
			      void *buf, unsigned int max)
{
	const void *data;
	unsigned int size = 0, n;

	(void)m;
	data = ramfs_lookup(path, &size);
	if (!data)
		return VFS_ENOENT;
	n = size > max ? max : size;
	mem_copy(buf, data, n);
	return (int)n;
}

static int ramfs_op_load_file(struct vfs_mount *m, const char *path,
			      struct vfs_file *out)
{
	const void *data;
	unsigned int size = 0;

	(void)m;
	data = ramfs_lookup(path, &size);
	if (!data)
		return VFS_ENOENT;
	out->data = data;
	out->size = size;
	return 0;
}

static void ramfs_op_release_file(struct vfs_mount *m, struct vfs_file *f)
{
	(void)m;
	(void)f;
}

static int ramfs_op_stat(struct vfs_mount *m, const char *path,
			 unsigned int *size, int *is_dir)
{
	const void *data;
	unsigned int sz = 0;
	struct ramfs_dirent e;

	(void)m;
	if (path[0] == '\0') {
		if (size)
			*size = 0;
		if (is_dir)
			*is_dir = 1;
		return 0;
	}
	data = ramfs_lookup(path, &sz);
	if (data) {
		if (size)
			*size = sz;
		if (is_dir)
			*is_dir = 0;
		return 0;
	}
	if (ramfs_list(path, 0, &e) == 1) {
		if (size)
			*size = 0;
		if (is_dir)
			*is_dir = 1;
		return 0;
	}
	return VFS_ENOENT;
}

static int ramfs_op_ro(struct vfs_mount *m, const char *path)
{
	(void)m;
	(void)path;
	return VFS_EROFS;
}

static int ramfs_op_write_file(struct vfs_mount *m, const char *path,
			       const void *buf, unsigned int len)
{
	(void)buf;
	(void)len;
	return ramfs_op_ro(m, path);
}

static int ramfs_op_unlink(struct vfs_mount *m, const char *path)
{
	return ramfs_op_ro(m, path);
}

static int ramfs_op_mkdir(struct vfs_mount *m, const char *path)
{
	return ramfs_op_ro(m, path);
}

static int ramfs_op_rmdir(struct vfs_mount *m, const char *path)
{
	return ramfs_op_ro(m, path);
}

static int ramfs_op_list_dir(struct vfs_mount *m, const char *path,
			     unsigned int index, struct vfs_dirent *out)
{
	struct ramfs_dirent e;

	(void)m;
	if (ramfs_list(path, index, &e) != 1) {
		if (index == 0 && path[0] != '\0') {
			unsigned int size = 0;

			/* Nothing matched: report a missing path instead of an
			 * empty directory (the pack has no empty directories --
			 * one exists exactly as a prefix of file paths), and a
			 * file as "not a directory". */
			if (ramfs_lookup(path, &size))
				return VFS_EINVAL;
			return VFS_ENOENT;
		}
		return 0;   /* end of directory */
	}
	copy_bounded(out->name, e.name, sizeof(out->name));
	out->size = e.size;
	out->is_dir = e.is_dir;
	return 1;
}

static const struct fs_ops ramfs_ops = {
	.raw_83 = 0,
	.read_file = ramfs_op_read_file,
	.load_file = ramfs_op_load_file,
	.release_file = ramfs_op_release_file,
	.stat = ramfs_op_stat,
	.write_file = ramfs_op_write_file,
	.unlink = ramfs_op_unlink,
	.mkdir = ramfs_op_mkdir,
	.rmdir = ramfs_op_rmdir,
	.list_dir = ramfs_op_list_dir,
};

/* ---- mounts ---- */

static vfs_mount_t *mount_slot(const char *name)
{
	unsigned int i;

	if (vfs_mount_count >= VFS_MAX_MOUNTS)
		return 0;
	{
		vfs_mount_t *m = &vfs_mounts[vfs_mount_count];

		for (i = 0; name[i] && i < sizeof(m->name) - 1; i++)
			m->name[i] = name[i];
		m->name[i] = '\0';
		m->mounted = 1;
		return m;
	}
}

static void mount_one(blk_dev_t *dev, unsigned int part_lba, const char *name)
{
	fat_fs_t fs;
	vfs_mount_t *m;

	if (fat_mount(dev, part_lba, &fs) != 0)
		return;
	m = mount_slot(name);
	if (!m)
		return;
	m->ops = &fat_ops;
	m->fs = fs;
	m->ro = 0;
	m->total_sectors = fs.total_sectors;
	copy_bounded(m->type, fat_name(fs.type), sizeof(m->type));
	copy_bounded(m->label, fs.label, sizeof(m->label));
	vfs_mount_count++;
}

void vfs_mount_ramfs(void)
{
	vfs_mount_t *m;

	if (ramfs_count() == 0)
		return;
	m = mount_slot("ramfs");
	if (!m)
		return;
	m->ops = &ramfs_ops;
	m->ro = 1;
	m->total_sectors = (ramfs_pack_size() + 511u) / 512u;
	copy_bounded(m->type, "ramfs", sizeof(m->type));
	m->label[0] = '\0';
	vfs_mount_count++;
}

void vfs_refresh_ramfs(void)
{
	vfs_mount_t *m = vfs_find("ramfs");

	if (m)
		m->total_sectors = (ramfs_pack_size() + 511u) / 512u;
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

	/* The ramfs is not a block device; keep it mounted across a refresh
	 * (the `storage` command calls vfs_mount_all()). */
	vfs_mount_ramfs();
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

		io_putstr(m->name);
		io_putstr("  ");
		io_putstr(m->type);
		io_putchar(' ');
		put_size_sectors(m->total_sectors);
		if (m->label[0]) {
			io_putstr("  \"");
			io_putstr(m->label);
			io_putchar('"');
		}
		if (m->ro)
			io_putstr("  (ro)");
		io_putchar('\n');
	}
}

int vfs_resolve(const char *path, struct vfs_mount **m_out,
		char *rel_path, unsigned int rel_size)
{
	unsigned int i = 0, j = 0, o = 0;
	char name[16];
	vfs_mount_t *m;

	if (!path || path[0] != '/' || rel_size == 0)
		return -1;

	i = 1;
	while (path[i] && path[i] != '/' && j < sizeof(name) - 1)
		name[j++] = path[i++];
	name[j] = '\0';

	if (j == 0)
		return -1;  /* path is just "/" */

	m = vfs_find(name);
	if (!m)
		return -1;

	/* Copy the remainder as a normalized relative path: skip slashes, drop
	 * "." components and never leave a trailing one. */
	while (path[i]) {
		unsigned int clen = 0;
		const char *comp;

		while (path[i] == '/')
			i++;
		if (!path[i])
			break;
		comp = path + i;
		while (path[i] && path[i] != '/') {
			i++;
			clen++;
		}
		if (clen == 1 && comp[0] == '.')
			continue;
		if (o + clen + 2 > rel_size)
			return -1;
		if (o > 0)
			rel_path[o++] = '/';
		{
			unsigned int k;
			for (k = 0; k < clen; k++)
				rel_path[o++] = comp[k];
		}
	}
	rel_path[o] = '\0';

	*m_out = m;
	return 0;
}

int vfs_load_file(const char *path, struct vfs_file *out,
		  struct vfs_mount **m_out)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	if (vfs_resolve(path, &m, rel, sizeof(rel)) != 0)
		return VFS_ENOENT;
	r = m->ops->load_file(m, rel, out);
	if (r == 0 && m_out)
		*m_out = m;
	return r;
}

void vfs_release_file(struct vfs_mount *m, struct vfs_file *f)
{
	if (m && m->ops->release_file)
		m->ops->release_file(m, f);
}

int vfs_stat(const char *path, unsigned int *size, int *is_dir)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];

	if (vfs_resolve(path, &m, rel, sizeof(rel)) != 0)
		return VFS_ENOENT;
	return m->ops->stat(m, rel, size, is_dir);
}

int vfs_list_dir(const char *path, unsigned int index, struct vfs_dirent *out)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];

	if (vfs_resolve(path, &m, rel, sizeof(rel)) != 0)
		return VFS_ENOENT;
	return m->ops->list_dir(m, rel, index, out);
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
