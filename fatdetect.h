/* Standalone FAT boot-sector + MBR detection, independent of the read/write
 * fat.c. Used by the `storage` command to identify filesystems on any device. */
#ifndef FATDETECT_H
#define FATDETECT_H

#include "fat.h"

typedef struct {
	fat_type_t type;
	unsigned int bytes_per_sector;
	unsigned int sectors_per_cluster;
	unsigned int total_sectors;   /* filesystem total sectors */
	unsigned int cluster_count;
	char label[12];               /* trimmed 8.3 volume label + NUL */
} fat_info_t;

/* Parse a 512-byte boot sector. Returns 0 and fills info, or -1. */
int fat_detect(const unsigned char *boot, fat_info_t *info);

typedef struct {
	unsigned char index;      /* 0..3 (original partition slot) */
	unsigned char type;       /* 0 = empty */
	unsigned int start_lba;
	unsigned int sectors;
	int is_fat;               /* 1 if type is a FAT partition type */
} mbr_part_t;

/* Parse an MBR (512 bytes). Returns number of non-empty partitions (0..4). */
int mbr_parse(const unsigned char *mbr, mbr_part_t parts[4]);

int mbr_is_fat_type(unsigned char type);

#endif
