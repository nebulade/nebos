/* FAT12/16/32 filesystem, device-parameterized (reads/writes via blk_read/blk_write). */

#include "fat.h"
#include "io.h"

#define SECTOR_SIZE     512
#define ROOT_ENTRY_SIZE 32

static unsigned char sector_buf[SECTOR_SIZE];

static unsigned int rd16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* ---- low-level sector access (fs-relative LBA) ---- */

static int fat_read_sector(fat_fs_t *fs, unsigned int lba)
{
	if (blk_read(fs->dev, fs->part_lba + lba, sector_buf, 1) != 0)
		return -2;
	return 0;
}

static int fat_write_sector(fat_fs_t *fs, unsigned int lba)
{
	if (blk_write(fs->dev, fs->part_lba + lba, sector_buf, 1) != 0)
		return -2;
	return 0;
}

static int fat_read_data(fat_fs_t *fs, unsigned int lba, void *buf, unsigned int count)
{
	if (blk_read(fs->dev, fs->part_lba + lba, buf, count) != 0)
		return -2;
	return 0;
}

/* ---- FAT entry access (12/16/32-bit) ---- */

static int fat_is_eoc(fat_fs_t *fs, unsigned int v)
{
	if (fs->type == FAT12) return v >= 0xFF8;
	if (fs->type == FAT16) return v >= 0xFFF8;
	return v >= 0x0FFFFFF8u;
}

static unsigned int fat_eoc(fat_fs_t *fs)
{
	if (fs->type == FAT32) return 0x0FFFFFFFu;
	if (fs->type == FAT16) return 0xFFFFu;
	return 0xFFFu;
}

static unsigned int fat_entry_offs(fat_fs_t *fs, unsigned int cluster,
				   unsigned int *nbytes)
{
	if (fs->type == FAT12) {
		*nbytes = 2;
		return (cluster * 3) / 2;
	}
	if (fs->type == FAT16) {
		*nbytes = 2;
		return cluster * 2;
	}
	*nbytes = 4;
	return cluster * 4;
}

static int fat_entry_get(fat_fs_t *fs, unsigned int cluster, unsigned int *value)
{
	unsigned int nbytes, byte_off, sec, within, i;
	unsigned char b[4];

	byte_off = fat_entry_offs(fs, cluster, &nbytes);
	sec = fs->reserved + byte_off / SECTOR_SIZE;
	within = byte_off % SECTOR_SIZE;

	if (fat_read_sector(fs, sec) != 0)
		return -2;
	for (i = 0; i < nbytes; i++) {
		unsigned int idx = within + i;
		if (idx < SECTOR_SIZE) {
			b[i] = sector_buf[idx];
		} else {
			if (idx == SECTOR_SIZE && fat_read_sector(fs, sec + 1) != 0)
				return -2;
			b[i] = sector_buf[idx - SECTOR_SIZE];
		}
	}

	if (fs->type == FAT12) {
		unsigned int w = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
		*value = (cluster & 1) ? (w >> 4) : (w & 0xFFF);
	} else if (fs->type == FAT16) {
		*value = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
	} else {
		*value = ((unsigned int)b[0] | ((unsigned int)b[1] << 8) |
			  ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24)) & 0x0FFFFFFFu;
	}
	return 0;
}

static int fat_entry_set(fat_fs_t *fs, unsigned int cluster, unsigned int value)
{
	unsigned int nbytes, byte_off, sec, within, i;
	unsigned char b[4];

	byte_off = fat_entry_offs(fs, cluster, &nbytes);
	sec = fs->reserved + byte_off / SECTOR_SIZE;
	within = byte_off % SECTOR_SIZE;

	/* Read current bytes (may load sec+1 on a straddle). */
	if (fat_read_sector(fs, sec) != 0)
		return -2;
	for (i = 0; i < nbytes; i++) {
		unsigned int idx = within + i;
		if (idx < SECTOR_SIZE) {
			b[i] = sector_buf[idx];
		} else {
			if (idx == SECTOR_SIZE && fat_read_sector(fs, sec + 1) != 0)
				return -2;
			b[i] = sector_buf[idx - SECTOR_SIZE];
		}
	}

	if (fs->type == FAT12) {
		value &= 0xFFF;
		if (cluster & 1) {
			b[0] = (unsigned char)((b[0] & 0x0F) | ((value << 4) & 0xF0));
			b[1] = (unsigned char)((value >> 4) & 0xFF);
		} else {
			b[0] = (unsigned char)(value & 0xFF);
			b[1] = (unsigned char)((b[1] & 0xF0) | ((value >> 8) & 0x0F));
		}
	} else if (fs->type == FAT16) {
		b[0] = (unsigned char)(value & 0xFF);
		b[1] = (unsigned char)((value >> 8) & 0xFF);
	} else {
		value &= 0x0FFFFFFFu;
		b[0] = (unsigned char)(value & 0xFF);
		b[1] = (unsigned char)((value >> 8) & 0xFF);
		b[2] = (unsigned char)((value >> 16) & 0xFF);
		b[3] = (unsigned char)((value >> 24) & 0xFF);
	}

	/* Write back (may straddle two sectors). */
	if (fat_read_sector(fs, sec) != 0)
		return -2;
	for (i = 0; i < nbytes && within + i < SECTOR_SIZE; i++)
		sector_buf[within + i] = b[i];
	if (fat_write_sector(fs, sec) != 0)
		return -2;
	if (within + nbytes > SECTOR_SIZE) {
		if (fat_read_sector(fs, sec + 1) != 0)
			return -2;
		for (; i < nbytes; i++)
			sector_buf[within + i - SECTOR_SIZE] = b[i];
		if (fat_write_sector(fs, sec + 1) != 0)
			return -2;
	}
	return 0;
}

/* ---- mount ---- */

int fat_mount(blk_dev_t *dev, unsigned int part_lba, fat_fs_t *fs)
{
	unsigned char *b = sector_buf;
	unsigned int root_entries, total, sec_per_fat16, sec_per_fat32;
	unsigned int root_dir_sectors, data_sectors, clusters, spc, media;
	unsigned int fsinfo_sec;

	if (!dev || !fs)
		return -1;

	fs->dev = dev;
	fs->part_lba = part_lba;
	fs->mounted = 0;

	if (blk_read(dev, part_lba, sector_buf, 1) != 0)
		return -2;

	if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA)
		return -1;

	fs->bytes_per_sector = rd16(b + 0x0B);
	if (fs->bytes_per_sector != 512)
		return -1;
	spc = b[0x0D];
	if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128)
		return -1;
	fs->sectors_per_cluster = spc;
	fs->reserved = rd16(b + 0x0E);
	if (fs->reserved < 1)
		return -1;
	fs->num_fats = b[0x10];
	if (fs->num_fats < 1 || fs->num_fats > 2)
		return -1;
	root_entries = rd16(b + 0x11);

	total = rd16(b + 0x13);
	if (total == 0)
		total = rd32(b + 0x20);
	if (total == 0)
		return -1;

	sec_per_fat16 = rd16(b + 0x16);
	sec_per_fat32 = rd32(b + 0x24);
	fs->sec_per_fat = sec_per_fat16 ? sec_per_fat16 : sec_per_fat32;
	if (fs->sec_per_fat == 0)
		return -1;

	root_dir_sectors = (root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (total < fs->reserved + fs->num_fats * fs->sec_per_fat + root_dir_sectors)
		return -1;
	data_sectors = total - fs->reserved - fs->num_fats * fs->sec_per_fat - root_dir_sectors;
	if (data_sectors < spc)
		return -1;
	clusters = data_sectors / spc;

	if (clusters < 4085)
		fs->type = FAT12;
	else if (clusters < 65525)
		fs->type = FAT16;
	else
		fs->type = FAT32;

	fs->root_entries = (fs->type == FAT32) ? 0 : root_entries;
	fs->root_dir_sectors = (fs->type == FAT32) ? 0 : root_dir_sectors;
	fs->root_cluster = (fs->type == FAT32) ? rd32(b + 0x2C) : 0;
	if (fs->type == FAT32 && fs->root_cluster < 2)
		return -1;
	fs->root_start = fs->reserved + fs->num_fats * fs->sec_per_fat;
	fs->data_start = fs->root_start + fs->root_dir_sectors;
	fs->total_sectors = total;
	fs->max_cluster = 2 + (total - fs->data_start) / spc;

	/* Volume label (11 bytes): 0x2B for FAT12/16, 0x47 for FAT32. */
	{
		unsigned int label_off = (fs->type == FAT32) ? 0x47 : 0x2B;
		unsigned int k;
		for (k = 0; k < 11; k++)
			fs->label[k] = (char)b[label_off + k];
		fs->label[11] = '\0';
		for (k = 11; k > 0 && (fs->label[k - 1] == ' ' || fs->label[k - 1] == '\0'); k--)
			;
		fs->label[k] = '\0';
	}

	/* Capture the FSInfo sector number before the media check clobbers
	 * sector_buf (see the allocation-hint block below). */
	fsinfo_sec = (fs->type == FAT32) ? rd16(b + 0x30) : 0;

	/* Validate the FAT media byte: rejects a boot disk whose BPB is fake
	 * (its FAT area is actually kernel code). */
	media = b[0x15];
	if (fat_read_sector(fs, fs->reserved) != 0)
		return -2;
	if (sector_buf[0] != media)
		return -1;

	/* Allocation hint: for FAT32 start from the FSInfo "next free cluster"
	 * when it is sane, so repeated writes do not rescan the whole FAT. */
	fs->next_free = 2;
	if (fs->type == FAT32 && fsinfo_sec != 0 &&
	    fat_read_sector(fs, fsinfo_sec) == 0 &&
	    rd32(sector_buf + 0x000) == 0x41615252u &&
	    rd32(sector_buf + 0x1FC) == 0xAA550000u) {
		unsigned int nf = rd32(sector_buf + 0x1EC);
		if (nf >= 2 && nf < fs->max_cluster)
			fs->next_free = nf;
	}

	fs->mounted = 1;
	return 0;
}

/* ---- name helpers (8.3) ---- */

static void trim_name_8_3(const char *in, char *out)
{
	unsigned int i;
	for (i = 0; i < 8 && in[i] && in[i] != ' '; i++)
		out[i] = (char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
	for (; i < 8; i++)
		out[i] = ' ';
	for (i = 0; i < 3 && in[8 + i] && in[8 + i] != ' '; i++)
		out[8 + i] = (char)(in[8 + i] >= 'a' && in[8 + i] <= 'z' ? in[8 + i] - 32 : in[8 + i]);
	for (; i < 3; i++)
		out[8 + i] = ' ';
	out[11] = '\0';
}

static void fat_name_to_8_3(const char *in, unsigned int len, unsigned char out[11])
{
	unsigned int i, j, dot;

	for (i = 0; i < len && in[i] == ' '; i++)
		;
	dot = i;
	while (dot < len && in[dot] != '.' && in[dot] != ' ')
		dot++;
	j = 0;
	while (i < dot && j < 8) {
		out[j++] = (unsigned char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
		i++;
	}
	while (j < 8)
		out[j++] = ' ';
	if (i < len && in[i] == '.')
		i++;
	while (i < len && in[i] != ' ' && j < 11) {
		out[j++] = (unsigned char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
		i++;
	}
	while (j < 11)
		out[j++] = ' ';
}

static unsigned int dir_entry_cluster(fat_fs_t *fs, const unsigned char *e)
{
	unsigned int c = (unsigned int)e[26] | ((unsigned int)e[27] << 8);
	if (fs->type == FAT32)
		c |= ((unsigned int)e[20] << 16) | ((unsigned int)e[21] << 24);
	return c;
}

/* ---- directory iteration ---- */

static int dir_each_entry(fat_fs_t *fs, unsigned int dir_cluster,
			  int (*cb)(unsigned int lba, unsigned int offset, void *ctx),
			  void *ctx)
{
	if (dir_cluster == 0) {
		/* FAT12/16 fixed root directory. */
		unsigned int sec;
		for (sec = 0; sec < fs->root_dir_sectors; sec++) {
			unsigned int off;
			if (fat_read_sector(fs, fs->root_start + sec) != 0)
				return -2;
			for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE)
				if (cb(fs->root_start + sec, off, ctx))
					return 0;
		}
	} else {
		unsigned int cluster = dir_cluster;
		while (cluster >= 2 && !fat_is_eoc(fs, cluster)) {
			unsigned int base = fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
			unsigned int i;
			for (i = 0; i < fs->sectors_per_cluster; i++) {
				unsigned int off;
				if (fat_read_sector(fs, base + i) != 0)
					return -2;
				for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE)
					if (cb(base + i, off, ctx))
						return 0;
			}
			if (fat_entry_get(fs, cluster, &cluster) != 0)
				return -2;
		}
	}
	return 0;
}

struct lookup_ctx {
	fat_fs_t *fs;
	const unsigned char *name;
	unsigned int cluster, size;
	unsigned char attr;
	unsigned int lba, offset;
	int found;
};

static int lookup_cb(unsigned int lba, unsigned int offset, void *vctx)
{
	struct lookup_ctx *c = vctx;
	unsigned char *e = sector_buf + offset;
	unsigned int i;

	if (e[0] == 0x00)
		return 1;  /* end of directory */
	if (e[0] == 0xE5)
		return 0;  /* deleted */
	if ((e[11] & 0x18) == 0x08)
		return 0;  /* volume label or LFN entry */
	for (i = 0; i < 11; i++)
		if ((e[i] >= 'a' ? e[i] - 32 : e[i]) != c->name[i])
			break;
	if (i == 11) {
		c->cluster = dir_entry_cluster(c->fs, e);
		c->size = (unsigned int)e[28] | ((unsigned int)e[29] << 8)
			| ((unsigned int)e[30] << 16) | ((unsigned int)e[31] << 24);
		c->attr = e[11];
		c->lba = lba;
		c->offset = offset;
		c->found = 1;
		return 1;
	}
	return 0;
}

static int fat_lookup(fat_fs_t *fs, unsigned int dir_cluster, const unsigned char name[11],
		      unsigned int *entry_cluster, unsigned int *size, unsigned char *attr,
		      unsigned int *entry_lba, unsigned int *entry_offset)
{
	struct lookup_ctx c;

	c.fs = fs;
	c.name = name;
	c.found = 0;
	c.cluster = c.size = 0;
	c.attr = 0;
	if (dir_each_entry(fs, dir_cluster, lookup_cb, &c) != 0)
		return -2;
	if (!c.found)
		return -1;
	if (entry_cluster) *entry_cluster = c.cluster;
	if (size) *size = c.size;
	if (attr) *attr = c.attr;
	if (entry_lba) *entry_lba = c.lba;
	if (entry_offset) *entry_offset = c.offset;
	return 0;
}

/* ---- public directory / read API ---- */

struct list_ctx {
	fat_root_callback_t callback;
};

static int list_cb(unsigned int lba, unsigned int offset, void *vctx)
{
	struct list_ctx *c = vctx;
	unsigned char *e = sector_buf + offset;
	char name_buf[12];
	unsigned int size;

	(void)lba;
	if (e[0] == 0x00)
		return 1;  /* end of directory */
	if (e[0] == 0xE5)
		return 0;
	if ((e[11] & 0x18) == 0x08)
		return 0;  /* volume label or LFN */
	trim_name_8_3((const char *)e, name_buf);
	size = (unsigned int)e[28] | ((unsigned int)e[29] << 8)
		| ((unsigned int)e[30] << 16) | ((unsigned int)e[31] << 24);
	c->callback(name_buf, (e[11] & 0x10) ? 1 : 0, size);
	return 0;
}

int fat_list_dir(fat_fs_t *fs, unsigned int dir_cluster, fat_root_callback_t callback)
{
	struct list_ctx c;

	c.callback = callback;
	if (!fs || !fs->mounted || !callback)
		return -1;
	return (dir_each_entry(fs, dir_cluster, list_cb, &c) != 0) ? -2 : 0;
}

int fat_read_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3,
		     void *buf, unsigned int max_len)
{
	unsigned int cluster, size;
	unsigned char attr;
	unsigned int copied = 0;
	unsigned char *dst = buf;
	int r;

	if (!fs || !fs->mounted || !buf || !name_8_3)
		return -1;

	r = fat_lookup(fs, dir_cluster, (const unsigned char *)name_8_3,
		       &cluster, &size, &attr, 0, 0);
	if (r == -2)
		return -2;
	if (r != 0 || (attr & 0x10))
		return -1;

	if (size > max_len)
		size = max_len;

	while (cluster >= 2 && !fat_is_eoc(fs, cluster) && copied < size) {
		unsigned int sector = fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
		unsigned int chunk = fs->sectors_per_cluster * SECTOR_SIZE;
		unsigned int n_sec;
		if (chunk > size - copied)
			chunk = size - copied;
		n_sec = (chunk + SECTOR_SIZE - 1) / SECTOR_SIZE;
		if (fat_read_data(fs, sector, dst + copied, n_sec) != 0)
			return -2;
		copied += chunk;
		if (copied >= size)
			break;
		if (fat_entry_get(fs, cluster, &cluster) != 0)
			return -2;
	}
	return (int)copied;
}

/* ---- write support ---- */

static int fat_find_free_cluster(fat_fs_t *fs)
{
	unsigned int c, start;

	/* Scan forward from the last allocation (wrapping once).  Without the
	 * hint a FAT32 root scan would re-read the whole FAT every time. */
	start = fs->next_free;
	if (start < 2 || start >= fs->max_cluster)
		start = 2;

	for (c = start; c < fs->max_cluster; c++) {
		unsigned int val;
		if (fat_entry_get(fs, c, &val) != 0)
			return -2;
		if (val == 0x000) {
			fs->next_free = c + 1;
			return (int)c;
		}
	}
	for (c = 2; c < start; c++) {
		unsigned int val;
		if (fat_entry_get(fs, c, &val) != 0)
			return -2;
		if (val == 0x000) {
			fs->next_free = c + 1;
			return (int)c;
		}
	}
	return -1;
}

static int fat_free_chain(fat_fs_t *fs, unsigned int cluster)
{
	while (cluster >= 2 && cluster < fs->max_cluster) {
		unsigned int next;
		if (fat_entry_get(fs, cluster, &next) != 0)
			return -2;
		if (fat_entry_set(fs, cluster, 0x000) != 0)
			return -2;
		if (cluster < fs->next_free)
			fs->next_free = cluster;
		if (next < 2 || fat_is_eoc(fs, next))
			break;
		cluster = next;
	}
	return 0;
}

struct free_ctx {
	unsigned int lba, offset;
	int found;
};

static int find_free_cb(unsigned int lba, unsigned int offset, void *vctx)
{
	struct free_ctx *c = vctx;
	if (sector_buf[offset] == 0x00 || sector_buf[offset] == 0xE5) {
		c->lba = lba;
		c->offset = offset;
		c->found = 1;
		return 1;
	}
	return 0;
}

static int dir_find_free(fat_fs_t *fs, unsigned int dir_cluster,
			 unsigned int *lba, unsigned int *offset)
{
	struct free_ctx c;

	c.found = 0;
	if (dir_each_entry(fs, dir_cluster, find_free_cb, &c) != 0)
		return -2;
	if (!c.found)
		return -1;
	*lba = c.lba;
	*offset = c.offset;
	return 0;
}

static int fat_dir_entry_write(fat_fs_t *fs, unsigned int lba, unsigned int offset,
			       const unsigned char name[11], unsigned char attr,
			       unsigned int cluster, unsigned int size)
{
	unsigned int i;

	if (fat_read_sector(fs, lba) != 0)
		return -2;

	for (i = 0; i < 11; i++)
		sector_buf[offset + i] = name[i];
	sector_buf[offset + 11] = attr;
	for (i = 12; i < 26; i++)
		sector_buf[offset + i] = 0;
	sector_buf[offset + 26] = (unsigned char)(cluster & 0xFF);
	sector_buf[offset + 27] = (unsigned char)((cluster >> 8) & 0xFF);
	if (fs->type == FAT32) {   /* high 16 bits of the first cluster */
		sector_buf[offset + 20] = (unsigned char)((cluster >> 16) & 0xFF);
		sector_buf[offset + 21] = (unsigned char)((cluster >> 24) & 0xFF);
	}
	sector_buf[offset + 28] = (unsigned char)(size & 0xFF);
	sector_buf[offset + 29] = (unsigned char)((size >> 8) & 0xFF);
	sector_buf[offset + 30] = (unsigned char)((size >> 16) & 0xFF);
	sector_buf[offset + 31] = (unsigned char)((size >> 24) & 0xFF);

	return (fat_write_sector(fs, lba) != 0) ? -2 : 0;
}

static int fat_mark_deleted(fat_fs_t *fs, unsigned int lba, unsigned int offset)
{
	if (fat_read_sector(fs, lba) != 0)
		return -2;
	sector_buf[offset] = 0xE5;
	return (fat_write_sector(fs, lba) != 0) ? -2 : 0;
}

int fat_write_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3,
		      const void *buf, unsigned int size)
{
	unsigned char name[11];
	unsigned int i;
	unsigned int first_cluster = 0, prev = 0, cluster;
	unsigned int remaining = size;
	const unsigned char *src = buf;
	unsigned int lba = 0, offset = 0, old_cluster = 0;
	unsigned char old_attr = 0;
	int r;

	if (!fs || !fs->mounted || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(fs, dir_cluster, name, &old_cluster, 0, &old_attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r == 0 && (old_attr & 0x10)) {
		io_putstr("write: is a directory\n");
		return -1;
	}
	if (r != 0) {
		r = dir_find_free(fs, dir_cluster, &lba, &offset);
		if (r == -2)
			return -2;
		if (r != 0) {
			io_putstr("write: directory full\n");
			return -1;
		}
	} else if (old_cluster >= 2) {
		fat_free_chain(fs, old_cluster);
	}

	if (remaining > 0) {
		unsigned int cluster_size = fs->sectors_per_cluster * SECTOR_SIZE;

		while (remaining > 0) {
			int c = fat_find_free_cluster(fs);
			if (c == -2)
				return -2;
			if (c < 0) {
				if (first_cluster)
					fat_free_chain(fs, first_cluster);
				io_putstr("write: disk full\n");
				return -1;
			}
			cluster = (unsigned int)c;
			if (first_cluster == 0)
				first_cluster = cluster;
			else if (fat_entry_set(fs, prev, cluster) != 0)
				return -2;

			{
				unsigned int chunk = (remaining < cluster_size) ? remaining : cluster_size;
				unsigned int lba2 = fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
				unsigned int full_secs = chunk / SECTOR_SIZE;
				unsigned int rem_bytes = chunk % SECTOR_SIZE;

				if (full_secs > 0) {
					if (blk_write(fs->dev, fs->part_lba + lba2, src, full_secs) != 0)
						return -2;
					src += full_secs * SECTOR_SIZE;
					lba2 += full_secs;
				}
				if (rem_bytes > 0) {
					unsigned int j;
					for (j = 0; j < rem_bytes; j++)
						sector_buf[j] = src[j];
					for (j = rem_bytes; j < SECTOR_SIZE; j++)
						sector_buf[j] = 0;
					if (blk_write(fs->dev, fs->part_lba + lba2, sector_buf, 1) != 0)
						return -2;
					src += rem_bytes;
				}
				remaining -= chunk;
			}
			prev = cluster;
		}
		if (fat_entry_set(fs, prev, fat_eoc(fs)) != 0)
			return -2;
	}

	if (fat_dir_entry_write(fs, lba, offset, name, 0x20, first_cluster, size) != 0)
		return -2;

	return 0;
}

int fat_delete_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster = 0;
	unsigned char attr = 0;
	unsigned int lba, offset;
	int r;

	if (!fs || !fs->mounted || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(fs, dir_cluster, name, &cluster, 0, &attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0)
		return -1;
	if (attr & 0x10)
		return -1;  /* it's a directory; use rmdir */

	if (cluster >= 2)
		fat_free_chain(fs, cluster);
	return fat_mark_deleted(fs, lba, offset);
}

static int dir_empty_cb(unsigned int lba, unsigned int offset, void *vctx)
{
	int *nonempty = vctx;
	unsigned char *e = sector_buf + offset;
	(void)lba;
	if (e[0] == 0x00 || e[0] == 0xE5)
		return 0;
	if (e[0] == '.')
		return 0;  /* "." or ".." */
	*nonempty = 1;
	return 1;
}

int fat_mkdir(fat_fs_t *fs, unsigned int parent_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster;
	unsigned int lba, offset;
	int r;

	if (!fs || !fs->mounted || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(fs, parent_cluster, name, 0, 0, 0, 0, 0);
	if (r == -2)
		return -2;
	if (r == 0) {
		io_putstr("mkdir: already exists\n");
		return -1;
	}

	r = fat_find_free_cluster(fs);
	if (r == -2)
		return -2;
	if (r < 0) {
		io_putstr("mkdir: disk full\n");
		return -1;
	}
	cluster = (unsigned int)r;

	if (fat_entry_set(fs, cluster, fat_eoc(fs)) != 0)
		return -2;

	{
		unsigned int base = fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
		for (i = 0; i < SECTOR_SIZE; i++)
			sector_buf[i] = 0;
		sector_buf[0] = '.';
		for (i = 1; i < 11; i++)
			sector_buf[i] = ' ';
		sector_buf[11] = 0x10;
		sector_buf[26] = (unsigned char)(cluster & 0xFF);
		sector_buf[27] = (unsigned char)((cluster >> 8) & 0xFF);
		sector_buf[32] = '.';
		sector_buf[33] = '.';
		for (i = 34; i < 43; i++)
			sector_buf[i] = ' ';
		sector_buf[43] = 0x10;
		sector_buf[58] = (unsigned char)(parent_cluster & 0xFF);
		sector_buf[59] = (unsigned char)((parent_cluster >> 8) & 0xFF);
		if (fs->type == FAT32) {   /* high halves of "." and ".." */
			sector_buf[20] = (unsigned char)((cluster >> 16) & 0xFF);
			sector_buf[21] = (unsigned char)((cluster >> 24) & 0xFF);
			sector_buf[52] = (unsigned char)((parent_cluster >> 16) & 0xFF);
			sector_buf[53] = (unsigned char)((parent_cluster >> 24) & 0xFF);
		}
		if (fat_write_sector(fs, base) != 0)
			return -2;
	}

	r = dir_find_free(fs, parent_cluster, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0) {
		fat_free_chain(fs, cluster);
		io_putstr("mkdir: directory full\n");
		return -1;
	}
	return fat_dir_entry_write(fs, lba, offset, name, 0x10, cluster, 0);
}

int fat_rmdir(fat_fs_t *fs, unsigned int parent_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster = 0;
	unsigned char attr = 0;
	unsigned int lba, offset;
	int r, nonempty = 0;

	if (!fs || !fs->mounted || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(fs, parent_cluster, name, &cluster, 0, &attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0)
		return -1;
	if (!(attr & 0x10)) {
		io_putstr("rmdir: not a directory\n");
		return -1;
	}

	if (dir_each_entry(fs, cluster, dir_empty_cb, &nonempty) != 0)
		return -2;
	if (nonempty) {
		io_putstr("rmdir: directory not empty\n");
		return -1;
	}

	if (cluster >= 2)
		fat_free_chain(fs, cluster);
	return fat_mark_deleted(fs, lba, offset);
}

/* ---- path resolution ---- */

int fat_resolve_path(fat_fs_t *fs, const char *path, unsigned int *dir_cluster,
		     char name_8_3[12])
{
	unsigned int dir = (fs->type == FAT32) ? fs->root_cluster : 0;
	const char *p = path;

	if (!fs || !fs->mounted || !path || !dir_cluster || !name_8_3)
		return -1;

	for (;;) {
		const char *start;
		unsigned int len;

		while (*p == '/')
			p++;
		if (!*p)
			return -1;  /* empty path */

		start = p;
		len = 0;
		while (p[len] && p[len] != '/')
			len++;

		if (p[len] == '/') {
			unsigned char name[11];
			unsigned int cluster, size;
			unsigned char attr;
			int r;
			fat_name_to_8_3(start, len, name);
			r = fat_lookup(fs, dir, name, &cluster, &size, &attr, 0, 0);
			if (r == -2)
				return -2;
			if (r != 0 || !(attr & 0x10))
				return -1;
			dir = cluster;
			p += len + 1;
		} else {
			fat_name_to_8_3(start, len, (unsigned char *)name_8_3);
			name_8_3[11] = '\0';
			*dir_cluster = dir;
			return 0;
		}
	}
}

int fat_resolve_dir(fat_fs_t *fs, const char *path, unsigned int *dir_cluster)
{
	unsigned int dir = (fs->type == FAT32) ? fs->root_cluster : 0;
	const char *p = path;

	if (!fs || !fs->mounted || !path || !dir_cluster)
		return -1;

	if (!*p) {
		*dir_cluster = dir;
		return 0;
	}

	for (;;) {
		const char *start;
		unsigned int len;

		while (*p == '/')
			p++;
		if (!*p)
			break;  /* trailing slash */

		start = p;
		len = 0;
		while (p[len] && p[len] != '/')
			len++;

		{
			unsigned char name[11];
			unsigned int cluster, size;
			unsigned char attr;
			int r;
			fat_name_to_8_3(start, len, name);
			r = fat_lookup(fs, dir, name, &cluster, &size, &attr, 0, 0);
			if (r == -2)
				return -2;
			if (r != 0 || !(attr & 0x10))
				return -1;
			dir = cluster;
		}
		if (!p[len])
			break;
		p += len + 1;
	}

	*dir_cluster = dir;
	return 0;
}

/* Stat a path: directory, then file.  See fat.h. */
int fat_stat(fat_fs_t *fs, const char *path, unsigned int *size, int *is_dir)
{
	unsigned int dir, sz;
	char name_8_3[12];

	if (fat_resolve_dir(fs, path, &dir) == 0) {
		if (size)
			*size = 0;
		if (is_dir)
			*is_dir = 1;
		return 0;
	}

	{
		int r = fat_resolve_path(fs, path, &dir, name_8_3);
		unsigned char attr;

		if (r == -2)
			return -2;
		if (r != 0)
			return -1;
		r = fat_lookup(fs, dir, (const unsigned char *)name_8_3, 0, &sz,
			       &attr, 0, 0);
		if (r == -2)
			return -2;
		if (r != 0)
			return -1;
		if (size)
			*size = sz;
		if (is_dir)
			*is_dir = (attr & 0x10) ? 1 : 0;
		return 0;
	}
}
