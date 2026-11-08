/* Kernel-state info syscalls (see sys_info.c). */
#ifndef SYS_INFO_H
#define SYS_INFO_H

#include "syscall.h"

int sys_meminfo(struct sys_meminfo *out);
int sys_e820(unsigned int index, struct sys_e820 *out);
int sys_mouseinfo(struct sys_mouseinfo *out);
int sys_storage_info(struct sys_storage_info *out);
int sys_blkdev(unsigned int index, struct sys_blkdev *out);
int sys_cputemp(struct sys_cputemp *out);
int sys_sensors(struct sys_sensors *out);
int sys_tpec_sensors(struct sys_tpec_sensors *out);
int sys_powerinfo(struct sys_powerinfo *out, unsigned int flags);

#endif
