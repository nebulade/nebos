/* FAT12 filesystem. Root directory only.
 * Error conventions:  0 = success, -1 = not found / not ready / full, -2 = disk I/O error. */

#ifndef FAT_H
#define FAT_H

typedef void (*fat_root_callback_t)(const char *name, int is_dir, unsigned int size);

/* Parse BPB from sector 0. Call once before list/read/write.
 * Returns 0 on success, -1 if no valid FAT filesystem, -2 on disk I/O error. */
int fat_init(void);

/* Iterate root directory, call callback for each file/dir.
 * Returns 0 on success, -1 if not ready, -2 on disk I/O error. */
int fat_list_root(fat_root_callback_t callback);

/* Read file by 8.3 name into buf, max max_len bytes.
 * Returns bytes read on success, -1 if not found, -2 on disk I/O error. */
int fat_read_file(const char *name_8_3, void *buf, unsigned int max_len);

/* Create or overwrite a root file with `size` bytes from `buf`.
 * Returns 0 on success, -1 if not ready / disk full, -2 on disk I/O error. */
int fat_write_file(const char *name_8_3, const void *buf, unsigned int size);

/* Delete a root file. Returns 0, -1 if not found, -2 on disk I/O error. */
int fat_delete_file(const char *name_8_3);

#endif
