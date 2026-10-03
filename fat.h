/* FAT12 filesystem with subdirectory support.
 * A "directory" is identified by its first data cluster; 0 means the root dir.
 * Error conventions:  0 = success, -1 = not found / not ready / full, -2 = disk I/O error. */

#ifndef FAT_H
#define FAT_H

typedef void (*fat_root_callback_t)(const char *name, int is_dir, unsigned int size);

/* Parse BPB from sector 0. Call once before list/read/write.
 * Returns 0 on success, -1 if no valid FAT filesystem, -2 on disk I/O error. */
int fat_init(void);

/* Iterate root directory, call callback for each file/dir. */
int fat_list_root(fat_root_callback_t callback);

/* Iterate a directory (0 = root, else first cluster). */
int fat_list_dir(unsigned int dir_cluster, fat_root_callback_t callback);

/* Read file by 8.3 name into buf, max max_len bytes (root only). */
int fat_read_file(const char *name_8_3, void *buf, unsigned int max_len);

/* Read a file in a directory (0 = root). */
int fat_read_file_in(unsigned int dir_cluster, const char *name_8_3, void *buf, unsigned int max_len);

/* Create or overwrite a file in the root directory. */
int fat_write_file(const char *name_8_3, const void *buf, unsigned int size);

/* Create or overwrite a file in a directory (0 = root). */
int fat_write_file_in(unsigned int dir_cluster, const char *name_8_3, const void *buf, unsigned int size);

/* Delete a file from the root directory. */
int fat_delete_file(const char *name_8_3);

/* Delete a file from a directory (0 = root). */
int fat_delete_file_in(unsigned int dir_cluster, const char *name_8_3);

/* Create a subdirectory in `parent_cluster` (0 = root). */
int fat_mkdir(unsigned int parent_cluster, const char *name_8_3);

/* Remove an empty subdirectory from `parent_cluster` (0 = root). */
int fat_rmdir(unsigned int parent_cluster, const char *name_8_3);

/* Resolve a path "a/b/c" into (parent dir cluster, final 8.3 name).
 * The returned name_8_3 is uppercase and space-padded (11 bytes + NUL). */
int fat_resolve_path(const char *path, unsigned int *dir_cluster, char name_8_3[12]);

/* Resolve a full path (all components directories) to a directory cluster.
 * An empty path resolves to the root (cluster 0). */
int fat_resolve_dir(const char *path, unsigned int *dir_cluster);

#endif
