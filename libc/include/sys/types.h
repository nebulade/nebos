/* Basic POSIX types for the ring-3 libc. */
#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int ssize_t;
typedef long off_t;
typedef long long time_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef unsigned int dev_t;
typedef unsigned int ino_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef long suseconds_t;

#endif
