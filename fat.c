/* FAT12 read-only: BPB parse, root list, read file by following FAT chain. */

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
#define BPB_SIGNATURE       0x1FE   /* 0x55 0xAA at end of boot sector */

static unsigned int fat_reserved;
static unsigned int fat_root_start;
static unsigned int fat_root_sectors;
static unsigned int fat_data_start;
static unsigned int fat_sec_per_cluster;
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
	unsigned int num_fats, root_entries, sec_per_fat;

	if (fat_read_sector(0) != 0) {
		io_putstr("run: disk read error (boot sector LBA=0)\n");
		return -2;  /* disk I/O error */
	}
	if (sector_buf[BPB_SIGNATURE] != 0x55 || sector_buf[BPB_SIGNATURE + 1] != 0xAA)
		return -1;  /* no valid FAT filesystem */
	fat_reserved = (unsigned int)sector_buf[BPB_RESERVED] | ((unsigned int)sector_buf[BPB_RESERVED + 1] << 8);
	num_fats = sector_buf[BPB_NUM_FATS];
	root_entries = (unsigned int)sector_buf[BPB_ROOT_ENTRIES] | ((unsigned int)sector_buf[BPB_ROOT_ENTRIES + 1] << 8);
	sec_per_fat = (unsigned int)sector_buf[BPB_SEC_PER_FAT] | ((unsigned int)sector_buf[BPB_SEC_PER_FAT + 1] << 8);
	fat_sec_per_cluster = sector_buf[BPB_SEC_PER_CLUSTER];

	fat_root_start = fat_reserved + num_fats * sec_per_fat;
	fat_root_sectors = (root_entries * ROOT_ENTRY_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
	fat_data_start = fat_root_start + fat_root_sectors;
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
