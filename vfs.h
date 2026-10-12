/* Virtual filesystem: filesystems mounted under `/`.  Two implementations:
 * FAT on block devices (probed by vfs_mount_all) and the read-only ramfs
 * backed by the command pack (ramfs.c, mounted at /ramfs). */
#ifndef VFS_H
#define VFS_H

#include "fat.h"

/* Largest file the shell will load from a filesystem into a kernel buffer. */
#define VFS_FILE_MAX (16 * 1024)

struct vfs_mount;
struct fs_ops;

struct vfs_file {
	const void *data;
	unsigned int size;
};

/* One directory entry: `name` is the raw 8.3 field for FAT (space padded) or
 * the plain file name for ramfs. */
struct vfs_dirent {
	char name[12];
	unsigned int size;
	unsigned int is_dir;
};

/* Per-filesystem error codes (0 = success; the read/list calls additionally
 * return positive byte/entry counts).  Mapped to SYS_E* by sys_fs.c. */
#define VFS_EIO    (-1)
#define VFS_ENOENT (-2)
#define VFS_EINVAL (-3)
#define VFS_EROFS  (-4)

/* Per-filesystem operations.  Paths are relative to the mount (no leading
 * '/'); "" is the mount root.  Implementations never print; list_dir returns
 * 1 (entry filled), 0 (end) or a negative VFS_E*. */
struct fs_ops {
	int raw_83;   /* dirent names are raw 8.3 (FAT) and need display fixing */
	int (*read_file)(struct vfs_mount *m, const char *path,
			 void *buf, unsigned int max);
	int (*load_file)(struct vfs_mount *m, const char *path,
			 struct vfs_file *out);
	void (*release_file)(struct vfs_mount *m, struct vfs_file *f);
	int (*stat)(struct vfs_mount *m, const char *path,
		    unsigned int *size, int *is_dir);
	int (*write_file)(struct vfs_mount *m, const char *path,
			  const void *buf, unsigned int len);
	int (*unlink)(struct vfs_mount *m, const char *path);
	int (*mkdir)(struct vfs_mount *m, const char *path);
	int (*rmdir)(struct vfs_mount *m, const char *path);
	int (*list_dir)(struct vfs_mount *m, const char *path,
			unsigned int index, struct vfs_dirent *out);
};

typedef struct vfs_mount {
	char name[16];
	const struct fs_ops *ops;
	int mounted;
	int ro;
	fat_fs_t fs;                 /* FAT mounts only */
	unsigned int total_sectors;  /* for the mount listing */
	char type[8];                /* "FAT12"/"FAT16"/"FAT32"/"ramfs" */
	char label[12];
} vfs_mount_t;

#define VFS_MAX_MOUNTS 24
#define VFS_PATH_MAX   64
extern vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
extern unsigned int vfs_mount_count;

/* Scan devices and mount all detected FAT filesystems. */
void vfs_mount_all(void);

/* Mount the command pack (ramfs) at /ramfs; no-op when no pack is loaded. */
void vfs_mount_ramfs(void);

/* Update the ramfs mount's reported size after ramfs_replace(). */
void vfs_refresh_ramfs(void);

/* Find a mount by name (NULL if not found). */
vfs_mount_t *vfs_find(const char *name);

/* Print the mount list (for `ls /`). */
void vfs_list_root(void);

/* Resolve an absolute path "/<mount>/<path>" into the mount and a normalized
 * path relative to it ("" for the mount root) in the caller's buffer.  `.`
 * components are dropped, repeated slashes collapsed and a trailing slash
 * removed, so directory paths from Tab completion ("/ramfs/bin/") work.
 * Returns 0 or -1 (bad path / unknown mount). */
int vfs_resolve(const char *path, struct vfs_mount **m,
		char *rel_path, unsigned int rel_size);

/* Load a whole file (borrowed pointer for ramfs, the static buffer for FAT);
 * release it with vfs_release_file() after use. */
int vfs_load_file(const char *path, struct vfs_file *out,
		  struct vfs_mount **m_out);
void vfs_release_file(struct vfs_mount *m, struct vfs_file *f);

/* Convenience wrappers for the shell and completion. */
int vfs_stat(const char *path, unsigned int *size, int *is_dir);
int vfs_list_dir(const char *path, unsigned int index, struct vfs_dirent *out);

/* Join `arg` onto `cwd` (arg may be absolute or relative) and normalize the
 * result into `out` as an absolute VFS path. "." and duplicate slashes are
 * dropped; ".." pops the previous component (popping the mount name yields
 * "/", and ".." at "/" stays "/"). Returns 0 on success, -1 if it doesn't fit. */
int vfs_join_path(const char *cwd, const char *arg, char *out, unsigned int out_size);

/* After a failed vfs_resolve, returns 1 if the path's first component names a
 * known block device (i.e. it exists but has no mounted filesystem). */
int vfs_device_known(const char *path);

#endif
