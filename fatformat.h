/* Create a fresh FAT filesystem on a block device (floppy = FAT12 superfloppy,
 * IDE = MBR + single FAT16/32 partition). */
#ifndef FATFORMAT_H
#define FATFORMAT_H

#include "blkdev.h"

/* Format device d. Returns 0 on success, -1 on error.  `limit_sectors`
 * (0 = the whole device) caps the partition size; useful for testing or for
 * reserving part of a large disk for another filesystem. */
int fat_format(blk_dev_t *d);
int fat_format_limited(blk_dev_t *d, unsigned int limit_sectors);

/* Decimal helper for the format messages (terminal + netlog mirror). */
void format_putdec(unsigned int v);

/* Set while a destructive format owns the disks.  Filesystem syscalls wait
 * for it instead of touching a half-written FAT; the owner is exempt. */
extern volatile int format_busy;
extern volatile unsigned int format_owner_pid;

#endif
