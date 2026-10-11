/* File-service syscalls (see sys_fs.c); used by syscall.c and the scheduler. */
#ifndef SYS_FS_H
#define SYS_FS_H

#include "syscall.h"

/* Set while a FAT operation is in progress: the scheduler must not switch
 * away from the running process (see sys_fs.c). */
extern volatile int sys_fs_busy;

/* Enter/leave the fs-busy section (used by other disk-touching services,
 * e.g. the storage info refresh). */
void sys_fs_enter(void);
void sys_fs_exit(void);

int sys_read_file(const char *path, void *buf, unsigned int max);
int sys_write_file(const char *path, const void *buf, unsigned int len);
int sys_unlink(const char *path);
int sys_mkdir(const char *path);
int sys_rmdir(const char *path);
int sys_list_dir(const char *path, unsigned int index, struct sys_dirent *out);
int sys_list_mounts(unsigned int index, struct sys_mount *out);

#endif
