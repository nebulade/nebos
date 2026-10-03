/* Disk layer: read sectors via direct FDC + ISA DMA in protected mode. */

#ifndef DISK_H
#define DISK_H

/* Read count sectors starting at lba into buf.
 * Returns 0 on success, -1 on error. */
int disk_read_sectors(unsigned int lba, void *buf, unsigned int count);

/* Write count sectors from buf starting at lba.
 * Returns 0 on success, -1 on error. */
int disk_write_sectors(unsigned int lba, const void *buf, unsigned int count);

#endif
