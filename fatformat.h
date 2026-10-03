/* Create a fresh FAT filesystem on a block device (floppy = FAT12 superfloppy,
 * IDE = MBR + single FAT16/32 partition). */
#ifndef FATFORMAT_H
#define FATFORMAT_H

#include "blkdev.h"

/* Format device d. Returns 0 on success, -1 on error. */
int fat_format(blk_dev_t *d);

#endif
