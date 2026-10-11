/* Syscall interface (int 0x80), shared by the kernel and ring-3 commands. */
#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PUTSTR       1
#define SYS_GET_TICKS_MS 2
#define SYS_SLEEP_MS     3
#define SYS_EXIT         4

/* File service (see sys_fs.c).  Paths are absolute VFS paths; buffers live in
 * the calling process's address space (the kernel runs on its CR3, so no copy
 * in/out is needed). */
#define SYS_READ_FILE    5   /* (const char *path, void *buf, unsigned int max)
				-> bytes read, or <0 (SYS_E*) */
#define SYS_WRITE_FILE   6   /* (const char *path, const void *buf, unsigned int len)
				-> 0, or <0 */
#define SYS_LIST_DIR     7   /* (const char *path, unsigned int index,
				struct sys_dirent *out) -> 1 entry, 0 end, <0 */
#define SYS_UNLINK       8   /* (const char *path) -> 0, or <0 */
#define SYS_MKDIR        9   /* (const char *path) -> 0, or <0 */
#define SYS_RMDIR       10   /* (const char *path) -> 0, or <0 */
#define SYS_LIST_MOUNTS 11   /* (unsigned int index, struct sys_mount *out)
				-> 1 mount, 0 end */

/* Error codes returned by the file syscalls. */
#define SYS_EIO     (-1)     /* disk I/O error */
#define SYS_ENODEV  (-2)     /* no such device / unusable path prefix */
#define SYS_ENOFS   (-3)     /* device exists but has no mounted filesystem */
#define SYS_ENOENT  (-4)     /* file/directory not found */
#define SYS_EINVAL  (-5)     /* path is not valid for this operation */
#define SYS_EWRITE  (-6)     /* write refused (full, or FAT rejected it) */

/* One directory entry, raw 8.3 name exactly as stored (space padded). */
struct sys_dirent {
	char name[12];
	unsigned int size;
	unsigned int is_dir;
};

/* One mounted filesystem (for `ls /`). */
struct sys_mount {
	char name[16];
	char type[8];           /* "FAT12", "FAT16", "FAT32" */
	unsigned int total_sectors;
	char label[12];
};

int syscall_dispatch(int num, int a1, int a2, int a3);

#endif
