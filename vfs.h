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

#define VFS_MAX_MOUNTS 24
#define VFS_PATH_MAX   64
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

/* Join `arg` onto `cwd` (arg may be absolute or relative) and normalize the
 * result into `out` as an absolute VFS path. "." and duplicate slashes are
 * dropped; ".." pops the previous component (popping the mount name yields
 * "/", and ".." at "/" stays "/"). Returns 0 on success, -1 if it doesn't fit. */
int vfs_join_path(const char *cwd, const char *arg, char *out, unsigned int out_size);

/* After a failed vfs_resolve, returns 1 if the path's first component names a
 * known block device (i.e. it exists but has no mounted filesystem). */
int vfs_device_known(const char *path);

#endif
