/* FAT12: BPB parse, root list, read/write files by following FAT chain. */

#include "disk.h"
#include "fat.h"
#include "io.h"

#define SECTOR_SIZE 512
#define ROOT_ENTRY_SIZE 32
#define FAT12_EOC_MIN 0xFF8

/* BPB offsets (same for FAT12 1.44MB floppy) */
#define BPB_BYTES_PER_SEC   0x0B
#define BPB_SEC_PER_CLUSTER 0x0D
#define BPB_RESERVED        0x0E
#define BPB_NUM_FATS        0x10
#define BPB_ROOT_ENTRIES    0x11
#define BPB_SEC_PER_FAT     0x16
#define BPB_TOTAL_SEC16     0x13   /* total sectors (16-bit) */
#define BPB_TOTAL_SEC32     0x20   /* total sectors (32-bit) */
#define BPB_SIGNATURE       0x1FE   /* 0x55 0xAA at end of boot sector */

static unsigned int fat_reserved;
static unsigned int fat_root_start;
static unsigned int fat_root_sectors;
static unsigned int fat_data_start;
static unsigned int fat_sec_per_cluster;
static unsigned int fat_num_fats;
static unsigned int fat_sec_per_fat;
static unsigned int fat_total_sectors;
static unsigned int fat_max_cluster;  /* 2 + number of data clusters */
static int fat_ready;

static unsigned char sector_buf[SECTOR_SIZE];

static int fat_read_sector(unsigned int lba)
{
	if (disk_read_sectors(lba, sector_buf, 1) != 0)
		return -1;
	return 0;
}

/* Get 12-bit FAT entry at cluster index. FAT is in sector_buf (one sector). */
static unsigned int fat_get_entry(unsigned int cluster_idx)
{
	unsigned int byte_off = (cluster_idx * 3) / 2;
	unsigned int word_val;
	if (byte_off + 2 > SECTOR_SIZE)
		return 0xFFFF;
	word_val = (unsigned int)sector_buf[byte_off] | ((unsigned int)sector_buf[byte_off + 1] << 8);
	if (cluster_idx & 1)
		return word_val >> 4;
	return word_val & 0xFFF;
}

int fat_init(void)
{
	unsigned int root_entries;

	if (fat_read_sector(0) != 0) {
		io_putstr("run: disk read error (boot sector LBA=0)\n");
		return -2;  /* disk I/O error */
	}
	if (sector_buf[BPB_SIGNATURE] != 0x55 || sector_buf[BPB_SIGNATURE + 1] != 0xAA)
		return -1;  /* no valid FAT filesystem */
	fat_reserved = (unsigned int)sector_buf[BPB_RESERVED] | ((unsigned int)sector_buf[BPB_RESERVED + 1] << 8);
	fat_num_fats = sector_buf[BPB_NUM_FATS];
	root_entries = (unsigned int)sector_buf[BPB_ROOT_ENTRIES] | ((unsigned int)sector_buf[BPB_ROOT_ENTRIES + 1] << 8);
	fat_sec_per_fat = (unsigned int)sector_buf[BPB_SEC_PER_FAT] | ((unsigned int)sector_buf[BPB_SEC_PER_FAT + 1] << 8);
	fat_sec_per_cluster = sector_buf[BPB_SEC_PER_CLUSTER];

	fat_total_sectors = (unsigned int)sector_buf[BPB_TOTAL_SEC16] | ((unsigned int)sector_buf[BPB_TOTAL_SEC16 + 1] << 8);
	if (fat_total_sectors == 0)
		fat_total_sectors = (unsigned int)sector_buf[BPB_TOTAL_SEC32]
			| ((unsigned int)sector_buf[BPB_TOTAL_SEC32 + 1] << 8)
			| ((unsigned int)sector_buf[BPB_TOTAL_SEC32 + 2] << 16)
			| ((unsigned int)sector_buf[BPB_TOTAL_SEC32 + 3] << 24);

	fat_root_start = fat_reserved + fat_num_fats * fat_sec_per_fat;
	fat_root_sectors = (root_entries * ROOT_ENTRY_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
	fat_data_start = fat_root_start + fat_root_sectors;

	if (fat_total_sectors > fat_data_start && fat_sec_per_cluster > 0)
		fat_max_cluster = 2 + (fat_total_sectors - fat_data_start) / fat_sec_per_cluster;
	else
		fat_max_cluster = 2;

	fat_ready = 1;
	return 0;
}

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

int fat_list_root(fat_root_callback_t callback)
{
	unsigned int sec, off;
	char name_buf[12];

	if (!fat_ready || !callback)
		return -1;
	for (sec = 0; sec < fat_root_sectors; sec++) {
		if (fat_read_sector(fat_root_start + sec) != 0) {
			io_putstr("run: disk read error (root dir sec=");
			io_puthex(sec);
			io_putstr(" LBA=");
			io_puthex(fat_root_start + sec);
			io_putstr(")\n");
			return -2;
		}
		for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE) {
			unsigned char *e = sector_buf + off;
			if (e[0] == 0x00)
				return 0;
			if (e[0] == 0xE5)
				continue;
			if ((e[11] & 0x18) == 0x08)
				continue;
			trim_name_8_3((const char *)e, name_buf);
			{
				unsigned int size = (unsigned int)e[28] | ((unsigned int)e[29] << 8) |
					((unsigned int)e[30] << 16) | ((unsigned int)e[31] << 24);
				(*callback)(name_buf, (e[11] & 0x10) ? 1 : 0, size);
			}
		}
	}
	return 0;
}

/* Find root entry by 8.3 name; return first cluster and size, or -1. */
static int fat_find_root(const char *name_8_3, unsigned int *out_cluster, unsigned int *out_size)
{
	unsigned int sec, off;
	char want[12];
	unsigned int i;

	for (i = 0; i < 11; i++)
		want[i] = (char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z' ? name_8_3[i] - 32 : name_8_3[i]);
	want[11] = '\0';

	for (sec = 0; sec < fat_root_sectors; sec++) {
		if (fat_read_sector(fat_root_start + sec) != 0) {
			io_putstr("run: disk read error (find root sec=");
			io_puthex(sec);
			io_putstr(" LBA=");
			io_puthex(fat_root_start + sec);
			io_putstr(")\n");
			return -2;
		}
		for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE) {
			unsigned char *e = sector_buf + off;
			if (e[0] == 0x00)
				return -1;
			if (e[0] == 0xE5 || (e[11] & 0x10))
				continue;
			for (i = 0; i < 11; i++)
				if ((e[i] >= 'a' ? e[i] - 32 : e[i]) != (unsigned char)want[i])
					break;
			if (i < 11)
				continue;
			*out_cluster = (unsigned int)e[26] | ((unsigned int)e[27] << 8);
			*out_size = (unsigned int)e[28] | ((unsigned int)e[29] << 8) |
				((unsigned int)e[30] << 16) | ((unsigned int)e[31] << 24);
			return 0;
		}
	}
	return -1;
}

int fat_read_file(const char *name_8_3, void *buf, unsigned int max_len)
{
	unsigned int cluster, size;
	unsigned int fat_start = fat_reserved;
	unsigned int copied = 0;
	unsigned char *dst = (unsigned char *)buf;
	unsigned int loaded_fat_sec;

	if (!fat_ready || !buf)
		return -1;
	{
		int fr = fat_find_root(name_8_3, &cluster, &size);
		if (fr == -2)
			return -2;
		if (fr != 0)
			return -1;
	}
	if (size > max_len)
		size = max_len;

	/* Pre-load the FAT sector covering the first cluster so we can follow
	 * the chain without re-reading the FAT between every data cluster.
	 * Data reads use DMA + memcpy to dst and never touch sector_buf,
	 * so the cached FAT data stays valid throughout. */
	loaded_fat_sec = fat_start + (cluster * 3) / (2 * SECTOR_SIZE);
	if (fat_read_sector(loaded_fat_sec) != 0) {
		io_putstr("run: disk read error (FAT preload LBA=");
		io_puthex(loaded_fat_sec);
		io_putstr(")\n");
		return -2;
	}

	while (cluster >= 2 && cluster < 0xFF8 && copied < size) {
		unsigned int sector = fat_data_start + (cluster - 2) * fat_sec_per_cluster;
		unsigned int chunk = fat_sec_per_cluster * SECTOR_SIZE;
		unsigned int n_sec;
		if (chunk > size - copied)
			chunk = size - copied;
		n_sec = (chunk + SECTOR_SIZE - 1) / SECTOR_SIZE;
		if (disk_read_sectors(sector, dst + copied, n_sec) != 0) {
			io_putstr("run: disk read error (file data cluster=");
			io_puthex(cluster);
			io_putstr(" LBA=");
			io_puthex(sector);
			io_putstr(" n_sec=");
			io_puthex(n_sec);
			io_putstr(")\n");
			return -2;
		}
		copied += chunk;
		if (copied >= size)
			break;
		/* Only re-read FAT if the chain crosses into a different FAT sector. */
		{
			unsigned int need_fat_sec = fat_start + (cluster * 3) / (2 * SECTOR_SIZE);
			if (need_fat_sec != loaded_fat_sec) {
				if (fat_read_sector(need_fat_sec) != 0) {
					io_putstr("run: disk read error (FAT cluster=");
					io_puthex(cluster);
					io_putstr(" LBA=");
					io_puthex(need_fat_sec);
					io_putstr(")\n");
					return -2;
				}
				loaded_fat_sec = need_fat_sec;
			}
		}
		cluster = fat_get_entry(cluster);
	}
	return (int)copied;
}

/* ---- write support ---- */

/* Read a 12-bit FAT entry for `cluster`, handling the case where the two
 * bytes straddle a FAT sector boundary. Returns 0, or -2 on disk error. */
static int fat_entry_read(unsigned int cluster, unsigned int *value)
{
	unsigned int byte_off = (cluster * 3) / 2;
	unsigned int sec = fat_reserved + byte_off / SECTOR_SIZE;
	unsigned int within = byte_off % SECTOR_SIZE;
	unsigned char b0, b1;

	if (within == SECTOR_SIZE - 1) {
		if (fat_read_sector(sec) != 0)
			return -2;
		b0 = sector_buf[within];
		if (fat_read_sector(sec + 1) != 0)
			return -2;
		b1 = sector_buf[0];
	} else {
		if (fat_read_sector(sec) != 0)
			return -2;
		b0 = sector_buf[within];
		b1 = sector_buf[within + 1];
	}

	if (cluster & 1)
		*value = ((unsigned int)b0 >> 4) | ((unsigned int)b1 << 4);
	else
		*value = (unsigned int)b0 | (((unsigned int)b1 & 0x0F) << 8);
	return 0;
}

/* Write a 12-bit FAT entry for `cluster` (read-modify-write), handling the
 * sector-boundary case. Returns 0, or -2 on disk error. */
static int fat_entry_write(unsigned int cluster, unsigned int value)
{
	unsigned int byte_off = (cluster * 3) / 2;
	unsigned int sec = fat_reserved + byte_off / SECTOR_SIZE;
	unsigned int within = byte_off % SECTOR_SIZE;

	value &= 0xFFF;

	if (within == SECTOR_SIZE - 1) {
		/* Entry straddles into the next FAT sector. */
		if (cluster & 1) {
			if (fat_read_sector(sec) != 0) return -2;
			sector_buf[within] = (sector_buf[within] & 0x0F) | (unsigned char)((value << 4) & 0xF0);
			if (disk_write_sectors(sec, sector_buf, 1) != 0) return -2;
			if (fat_read_sector(sec + 1) != 0) return -2;
			sector_buf[0] = (unsigned char)((value >> 4) & 0xFF);
			if (disk_write_sectors(sec + 1, sector_buf, 1) != 0) return -2;
		} else {
			if (fat_read_sector(sec) != 0) return -2;
			sector_buf[within] = (unsigned char)(value & 0xFF);
			if (disk_write_sectors(sec, sector_buf, 1) != 0) return -2;
			if (fat_read_sector(sec + 1) != 0) return -2;
			sector_buf[0] = (sector_buf[0] & 0xF0) | (unsigned char)((value >> 8) & 0x0F);
			if (disk_write_sectors(sec + 1, sector_buf, 1) != 0) return -2;
		}
	} else {
		if (fat_read_sector(sec) != 0) return -2;
		if (cluster & 1) {
			sector_buf[within] = (sector_buf[within] & 0x0F) | (unsigned char)((value << 4) & 0xF0);
			sector_buf[within + 1] = (unsigned char)((value >> 4) & 0xFF);
		} else {
			sector_buf[within] = (unsigned char)(value & 0xFF);
			sector_buf[within + 1] = (sector_buf[within + 1] & 0xF0) | (unsigned char)((value >> 8) & 0x0F);
		}
		if (disk_write_sectors(sec, sector_buf, 1) != 0) return -2;
	}
	return 0;
}

/* Find a free data cluster (FAT entry == 0x000). Returns cluster >= 2, -1 if
 * the disk is full, -2 on disk error. */
static int fat_find_free_cluster(void)
{
	unsigned int cluster;

	for (cluster = 2; cluster < fat_max_cluster; cluster++) {
		unsigned int val;
		if (fat_entry_read(cluster, &val) != 0)
			return -2;
		if (val == 0x000)
			return (int)cluster;
	}
	return -1;
}

/* Mark every cluster in a chain as free (0x000). */
static int fat_free_chain(unsigned int cluster)
{
	while (cluster >= 2 && cluster < fat_max_cluster) {
		unsigned int next;
		if (fat_entry_read(cluster, &next) != 0)
			return -2;
		if (fat_entry_write(cluster, 0x000) != 0)
			return -2;
		if (next < 2 || next >= FAT12_EOC_MIN)
			break;
		cluster = next;
	}
	return 0;
}

/* Convert a name (length `len`) into an 11-byte 8.3 name (uppercase, padded). */
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

/* Iterate the entries of a directory (0 = root, else first cluster). For each
 * entry load the sector and call cb(lba, offset, ctx); cb returns non-zero to
 * stop. Returns 0, or -2 on disk error. */
static int dir_each_entry(unsigned int dir_cluster,
			  int (*cb)(unsigned int lba, unsigned int offset, void *ctx),
			  void *ctx)
{
	if (dir_cluster == 0) {
		unsigned int sec;
		for (sec = 0; sec < fat_root_sectors; sec++) {
			unsigned int off;
			if (fat_read_sector(fat_root_start + sec) != 0)
				return -2;
			for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE)
				if (cb(fat_root_start + sec, off, ctx))
					return 0;
		}
	} else {
		unsigned int cluster = dir_cluster;
		while (cluster >= 2 && cluster < FAT12_EOC_MIN) {
			unsigned int base = fat_data_start + (cluster - 2) * fat_sec_per_cluster;
			unsigned int i;
			for (i = 0; i < fat_sec_per_cluster; i++) {
				unsigned int off;
				if (fat_read_sector(base + i) != 0)
					return -2;
				for (off = 0; off < SECTOR_SIZE; off += ROOT_ENTRY_SIZE)
					if (cb(base + i, off, ctx))
						return 0;
			}
			if (fat_entry_read(cluster, &cluster) != 0)
				return -2;
		}
	}
	return 0;
}

struct lookup_ctx {
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
	for (i = 0; i < 11; i++)
		if ((e[i] >= 'a' ? e[i] - 32 : e[i]) != c->name[i])
			break;
	if (i == 11) {
		c->cluster = (unsigned int)e[26] | ((unsigned int)e[27] << 8);
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

/* Look up `name` (11-byte, uppercase) in directory `dir_cluster`. */
static int fat_lookup(unsigned int dir_cluster, const unsigned char name[11],
		      unsigned int *entry_cluster, unsigned int *size, unsigned char *attr,
		      unsigned int *entry_lba, unsigned int *entry_offset)
{
	struct lookup_ctx c;

	c.name = name;
	c.found = 0;
	c.cluster = c.size = 0;
	c.attr = 0;
	if (dir_each_entry(dir_cluster, lookup_cb, &c) != 0)
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

/* Find a free (0x00 or 0xE5) entry in `dir_cluster`. Returns 0 and fills
 * lba/offset, or -1 if the directory is full, -2 on disk error. */
static int dir_find_free(unsigned int dir_cluster, unsigned int *lba, unsigned int *offset)
{
	struct free_ctx c;
	c.found = 0;
	if (dir_each_entry(dir_cluster, find_free_cb, &c) != 0)
		return -2;
	if (!c.found)
		return -1;
	*lba = c.lba;
	*offset = c.offset;
	return 0;
}

/* Write a 32-byte directory entry at (lba, offset). */
static int fat_dir_entry_write(unsigned int lba, unsigned int offset,
			       const unsigned char name[11], unsigned char attr,
			       unsigned int cluster, unsigned int size)
{
	unsigned int i;

	if (fat_read_sector(lba) != 0)
		return -2;

	for (i = 0; i < 11; i++)
		sector_buf[offset + i] = name[i];
	sector_buf[offset + 11] = attr;
	for (i = 12; i < 26; i++)
		sector_buf[offset + i] = 0;
	sector_buf[offset + 26] = (unsigned char)(cluster & 0xFF);
	sector_buf[offset + 27] = (unsigned char)((cluster >> 8) & 0xFF);
	sector_buf[offset + 28] = (unsigned char)(size & 0xFF);
	sector_buf[offset + 29] = (unsigned char)((size >> 8) & 0xFF);
	sector_buf[offset + 30] = (unsigned char)((size >> 16) & 0xFF);
	sector_buf[offset + 31] = (unsigned char)((size >> 24) & 0xFF);

	return (disk_write_sectors(lba, sector_buf, 1) != 0) ? -2 : 0;
}

/* Mark the entry at (lba, offset) deleted (0xE5). */
static int fat_mark_deleted(unsigned int lba, unsigned int offset)
{
	if (fat_read_sector(lba) != 0)
		return -2;
	sector_buf[offset] = 0xE5;
	return (disk_write_sectors(lba, sector_buf, 1) != 0) ? -2 : 0;
}

/* ---- public directory API ---- */

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
		return 0;  /* volume label */
	trim_name_8_3((const char *)e, name_buf);
	size = (unsigned int)e[28] | ((unsigned int)e[29] << 8)
		| ((unsigned int)e[30] << 16) | ((unsigned int)e[31] << 24);
	c->callback(name_buf, (e[11] & 0x10) ? 1 : 0, size);
	return 0;
}

int fat_list_dir(unsigned int dir_cluster, fat_root_callback_t callback)
{
	struct list_ctx c;
	c.callback = callback;
	if (!fat_ready || !callback)
		return -1;
	return (dir_each_entry(dir_cluster, list_cb, &c) != 0) ? -2 : 0;
}

int fat_read_file_in(unsigned int dir_cluster, const char *name_8_3, void *buf, unsigned int max_len)
{
	unsigned int cluster, size;
	unsigned char attr;
	unsigned int copied = 0;
	unsigned char *dst = buf;
	int r;

	if (!fat_ready || !buf || !name_8_3)
		return -1;

	r = fat_lookup(dir_cluster, (const unsigned char *)name_8_3, &cluster, &size, &attr, 0, 0);
	if (r == -2)
		return -2;
	if (r != 0 || (attr & 0x10))
		return -1;

	if (size > max_len)
		size = max_len;

	while (cluster >= 2 && cluster < FAT12_EOC_MIN && copied < size) {
		unsigned int sector = fat_data_start + (cluster - 2) * fat_sec_per_cluster;
		unsigned int chunk = fat_sec_per_cluster * SECTOR_SIZE;
		unsigned int n_sec;
		if (chunk > size - copied)
			chunk = size - copied;
		n_sec = (chunk + SECTOR_SIZE - 1) / SECTOR_SIZE;
		if (disk_read_sectors(sector, dst + copied, n_sec) != 0)
			return -2;
		copied += chunk;
		if (copied >= size)
			break;
		if (fat_entry_read(cluster, &cluster) != 0)
			return -2;
	}
	return (int)copied;
}

int fat_write_file_in(unsigned int dir_cluster, const char *name_8_3, const void *buf, unsigned int size)
{
	unsigned char name[11];
	unsigned int i;
	unsigned int first_cluster = 0, prev = 0, cluster;
	unsigned int remaining = size;
	const unsigned char *src = buf;
	unsigned int lba = 0, offset = 0, old_cluster = 0;
	unsigned char old_attr = 0;
	int r;

	if (!fat_ready || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(dir_cluster, name, &old_cluster, 0, &old_attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r == 0 && (old_attr & 0x10)) {
		io_putstr("write: is a directory\n");
		return -1;
	}
	if (r != 0) {
		/* not found: find a free entry */
		r = dir_find_free(dir_cluster, &lba, &offset);
		if (r == -2)
			return -2;
		if (r != 0) {
			io_putstr("write: directory full\n");
			return -1;
		}
	} else if (old_cluster >= 2) {
		/* overwrite: free the old chain */
		fat_free_chain(old_cluster);
	}

	if (remaining > 0) {
		unsigned int cluster_size = fat_sec_per_cluster * SECTOR_SIZE;

		while (remaining > 0) {
			int c = fat_find_free_cluster();
			if (c == -2)
				return -2;
			if (c < 0) {
				if (first_cluster)
					fat_free_chain(first_cluster);
				io_putstr("write: disk full\n");
				return -1;
			}
			cluster = (unsigned int)c;
			if (first_cluster == 0)
				first_cluster = cluster;
			else if (fat_entry_write(prev, cluster) != 0)
				return -2;

			{
				unsigned int chunk = (remaining < cluster_size) ? remaining : cluster_size;
				unsigned int lba2 = fat_data_start + (cluster - 2) * fat_sec_per_cluster;
				unsigned int full_secs = chunk / SECTOR_SIZE;
				unsigned int rem_bytes = chunk % SECTOR_SIZE;

				if (full_secs > 0) {
					if (disk_write_sectors(lba2, src, full_secs) != 0)
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
					if (disk_write_sectors(lba2, sector_buf, 1) != 0)
						return -2;
					src += rem_bytes;
				}
				remaining -= chunk;
			}
			prev = cluster;
		}
		if (fat_entry_write(prev, 0xFFF) != 0)
			return -2;
	}

	if (fat_dir_entry_write(lba, offset, name, 0x20, first_cluster, size) != 0)
		return -2;

	return 0;
}

int fat_delete_file_in(unsigned int dir_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster = 0;
	unsigned char attr = 0;
	unsigned int lba, offset;
	int r;

	if (!fat_ready || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(dir_cluster, name, &cluster, 0, &attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0)
		return -1;
	if (attr & 0x10)
		return -1;  /* it's a directory; use rmdir */

	if (cluster >= 2)
		fat_free_chain(cluster);
	return fat_mark_deleted(lba, offset);
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

int fat_mkdir(unsigned int parent_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster;
	unsigned int lba, offset;
	int r;

	if (!fat_ready || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(parent_cluster, name, 0, 0, 0, 0, 0);
	if (r == -2)
		return -2;
	if (r == 0) {
		io_putstr("mkdir: already exists\n");
		return -1;
	}

	r = fat_find_free_cluster();
	if (r == -2)
		return -2;
	if (r < 0) {
		io_putstr("mkdir: disk full\n");
		return -1;
	}
	cluster = (unsigned int)r;

	/* Mark the cluster allocated (end of chain) so it's not reused. */
	if (fat_entry_write(cluster, 0xFFF) != 0)
		return -2;

	/* Write "." and ".." entries into the cluster's first sector. */
	{
		unsigned int base = fat_data_start + (cluster - 2) * fat_sec_per_cluster;
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
		if (disk_write_sectors(base, sector_buf, 1) != 0)
			return -2;
	}

	r = dir_find_free(parent_cluster, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0) {
		fat_free_chain(cluster);
		io_putstr("mkdir: directory full\n");
		return -1;
	}
	return fat_dir_entry_write(lba, offset, name, 0x10, cluster, 0);
}

int fat_rmdir(unsigned int parent_cluster, const char *name_8_3)
{
	unsigned char name[11];
	unsigned int i, cluster = 0;
	unsigned char attr = 0;
	unsigned int lba, offset;
	int r, nonempty = 0;

	if (!fat_ready || !name_8_3)
		return -1;

	for (i = 0; i < 11; i++)
		name[i] = (unsigned char)(name_8_3[i] >= 'a' && name_8_3[i] <= 'z'
			? name_8_3[i] - 32 : name_8_3[i]);

	r = fat_lookup(parent_cluster, name, &cluster, 0, &attr, &lba, &offset);
	if (r == -2)
		return -2;
	if (r != 0)
		return -1;
	if (!(attr & 0x10)) {
		io_putstr("rmdir: not a directory\n");
		return -1;
	}

	if (dir_each_entry(cluster, dir_empty_cb, &nonempty) != 0)
		return -2;
	if (nonempty) {
		io_putstr("rmdir: directory not empty\n");
		return -1;
	}

	if (cluster >= 2)
		fat_free_chain(cluster);
	return fat_mark_deleted(lba, offset);
}

/* Resolve a path "a/b/c" into (parent dir cluster, final 8.3 name). */
int fat_resolve_path(const char *path, unsigned int *dir_cluster, char name_8_3[12])
{
	unsigned int dir = 0;
	const char *p = path;

	if (!fat_ready || !path || !dir_cluster || !name_8_3)
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
			/* intermediate component: must be an existing directory */
			unsigned char name[11];
			unsigned int cluster, size;
			unsigned char attr;
			int r;
			fat_name_to_8_3(start, len, name);
			r = fat_lookup(dir, name, &cluster, &size, &attr, 0, 0);
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

/* Resolve a full path (all components must be directories) to a directory
 * cluster. An empty path resolves to the root (cluster 0). */
int fat_resolve_dir(const char *path, unsigned int *dir_cluster)
{
	unsigned int dir = 0;
	const char *p = path;

	if (!fat_ready || !path || !dir_cluster)
		return -1;

	if (!*p) {
		*dir_cluster = 0;
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
			r = fat_lookup(dir, name, &cluster, &size, &attr, 0, 0);
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

/* Root-directory wrappers (dir_cluster = 0). */
int fat_write_file(const char *name_8_3, const void *buf, unsigned int size)
{
	return fat_write_file_in(0, name_8_3, buf, size);
}

int fat_delete_file(const char *name_8_3)
{
	return fat_delete_file_in(0, name_8_3);
}
