/* Virtual filesystem: mounts every device with a valid FAT filesystem under
 * `/` (e.g. /floppy0, /floppy1, /ide0p1). Provides path resolution so the
 * shell commands operate on the right filesystem. */
#ifndef VFS_H
#define VFS_H

#include "fat.h"

typedef struct {
	char name[16];
	fat_fs_t fs;
	int mounted;
} vfs_mount_t;

#define VFS_MAX_MOUNTS 12
extern vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
extern unsigned int vfs_mount_count;

/* Scan devices and mount all detected FAT filesystems. */
void vfs_mount_all(void);

/* Find a mount by name (NULL if not found). */
vfs_mount_t *vfs_find(const char *name);

/* Print the mount list (for `ls /`). */
void vfs_list_root(void);

/* Resolve an absolute path "/<mount>/<path>" into (fs, fs_path). The fs_path
 * points into `path` (no copy). Returns 0 on success, -1 on error. */
int vfs_resolve(const char *path, fat_fs_t **fs, const char **fs_path);

#endif
