/* FAT12/16/32 filesystem with subdirectory support, device-parameterized.
 * A "directory" is identified by a cluster: 0 = root for FAT12/16,
 * root_cluster (2) = root for FAT32, else a subdirectory's first cluster.
 * Error conventions: 0 = success, -1 = not found / not ready / full / invalid,
 * -2 = disk I/O error. */

#ifndef FAT_H
#define FAT_H

#include "blkdev.h"

typedef enum { FAT_NONE, FAT12, FAT16, FAT32 } fat_type_t;

typedef void (*fat_root_callback_t)(const char *name, int is_dir, unsigned int size);

typedef struct {
	blk_dev_t *dev;
	unsigned int part_lba;          /* LBA of the filesystem (partition) start */
	fat_type_t type;
	unsigned int bytes_per_sector;
	unsigned int sectors_per_cluster;
	unsigned int reserved;
	unsigned int num_fats;
	unsigned int sec_per_fat;
	unsigned int root_entries;      /* 0 for FAT32 */
	unsigned int root_dir_sectors;
	unsigned int root_cluster;      /* FAT32 root (2); 0 otherwise */
	unsigned int root_start;        /* LBA of root dir (FAT12/16) */
	unsigned int data_start;        /* LBA of first data cluster */
	unsigned int total_sectors;
	unsigned int max_cluster;
	char label[12];                /* trimmed 8.3 volume label + NUL */
	int mounted;
} fat_fs_t;

/* Mount (parse BPB + validate FAT media byte) a filesystem at part_lba on dev.
 * Returns 0 on success, -1 if no valid FAT, -2 on I/O error. */
int fat_mount(blk_dev_t *dev, unsigned int part_lba, fat_fs_t *fs);

/* Iterate a directory (root or subdir), call callback for each entry. */
int fat_list_dir(fat_fs_t *fs, unsigned int dir_cluster, fat_root_callback_t callback);

/* Read a file by 8.3 name in a directory. */
int fat_read_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3,
		     void *buf, unsigned int max_len);

/* Write / delete / mkdir / rmdir. Return -1 if FAT32 (read-only). */
int fat_write_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3,
		      const void *buf, unsigned int size);
int fat_delete_file_in(fat_fs_t *fs, unsigned int dir_cluster, const char *name_8_3);
int fat_mkdir(fat_fs_t *fs, unsigned int parent_cluster, const char *name_8_3);
int fat_rmdir(fat_fs_t *fs, unsigned int parent_cluster, const char *name_8_3);

/* Resolve a path "a/b/c" into (parent dir cluster, final 8.3 name). */
int fat_resolve_path(fat_fs_t *fs, const char *path, unsigned int *dir_cluster,
		     char name_8_3[12]);

/* Resolve a full path (all components directories) to a directory cluster.
 * An empty path resolves to the root. */
int fat_resolve_dir(fat_fs_t *fs, const char *path, unsigned int *dir_cluster);

#endif
