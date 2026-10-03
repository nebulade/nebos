/* FAT boot-sector + MBR detection (FAT12/16/32). */

#include "fatdetect.h"

static unsigned int rd16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int is_pow2(unsigned int v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

int fat_detect(const unsigned char *boot, fat_info_t *info)
{
	unsigned int bps, spc, reserved, num_fats, root_entries;
	unsigned int total, sec_per_fat16, sec_per_fat32, sec_per_fat;
	unsigned int root_dir_sectors, data_sectors, clusters;
	unsigned int label_off, i;

	if (boot[510] != 0x55 || boot[511] != 0xAA)
		return -1;

	bps = rd16(boot + 0x0B);
	if (bps != 512)
		return -1;
	spc = boot[0x0D];
	if (!is_pow2(spc) || spc > 128)
		return -1;
	reserved = rd16(boot + 0x0E);
	if (reserved < 1)
		return -1;
	num_fats = boot[0x10];
	if (num_fats < 1 || num_fats > 2)
		return -1;
	root_entries = rd16(boot + 0x11);

	total = rd16(boot + 0x13);
	if (total == 0)
		total = rd32(boot + 0x20);
	if (total == 0)
		return -1;

	sec_per_fat16 = rd16(boot + 0x16);
	sec_per_fat32 = rd32(boot + 0x24);
	sec_per_fat = sec_per_fat16 ? sec_per_fat16 : sec_per_fat32;
	if (sec_per_fat == 0)
		return -1;

	root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
	if (total < reserved + num_fats * sec_per_fat + root_dir_sectors)
		return -1;
	data_sectors = total - reserved - num_fats * sec_per_fat - root_dir_sectors;
	if (data_sectors < spc)
		return -1;
	clusters = data_sectors / spc;

	if (clusters < 4085)
		info->type = FAT12;
	else if (clusters < 65525)
		info->type = FAT16;
	else
		info->type = FAT32;

	info->bytes_per_sector = bps;
	info->sectors_per_cluster = spc;
	info->total_sectors = total;
	info->cluster_count = clusters;

	/* Volume label (11 bytes): 0x2B for FAT12/16, 0x47 for FAT32. */
	label_off = (info->type == FAT32) ? 0x47 : 0x2B;
	for (i = 0; i < 11; i++)
		info->label[i] = (char)boot[label_off + i];
	info->label[11] = '\0';
	for (i = 11; i > 0 && (info->label[i - 1] == ' ' || info->label[i - 1] == '\0'); i--)
		;
	info->label[i] = '\0';

	return 0;
}

int mbr_is_fat_type(unsigned char type)
{
	switch (type) {
	case 0x01: case 0x04: case 0x06:
	case 0x0B: case 0x0C: case 0x0E:
		return 1;
	default:
		return 0;
	}
}

int mbr_parse(const unsigned char *mbr, mbr_part_t parts[4])
{
	int n = 0, i;

	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return 0;

	for (i = 0; i < 4; i++) {
		const unsigned char *e = mbr + 0x1BE + (unsigned int)i * 16;
		unsigned char type = e[4];
		if (type == 0)
			continue;
		parts[n].index = (unsigned char)i;
		parts[n].type = type;
		parts[n].start_lba = rd32(e + 8);
		parts[n].sectors = rd32(e + 12);
		parts[n].is_fat = mbr_is_fat_type(type);
		n++;
	}
	return n;
}
