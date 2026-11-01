/* Disk layer: read sectors via direct FDC + ISA DMA in protected mode. */

#ifndef DISK_H
#define DISK_H

/* Select the floppy drive (0 or 1) used by subsequent disk_read_sectors calls.
 * Keeps the controller initialised; the new drive is spun up and recalibrated
 * only when needed. */
void disk_set_drive(unsigned int drive);

/* Quick boot-scan presence/media probe: one read with a short timeout, no
 * recovery retries.  Returns 1 if a readable disk is in the drive, 0
 * otherwise.  Normal I/O after a successful probe still gets full retries. */
int disk_probe(unsigned int drive);

/* Return currently selected drive number. */
unsigned int disk_get_drive(void);

/* Read count sectors starting at lba into buf.
 * Returns 0 on success, -1 on error. */
int disk_read_sectors(unsigned int lba, void *buf, unsigned int count);

/* Write count sectors from buf starting at lba.
 * Returns 0 on success, -1 on error. */
int disk_write_sectors(unsigned int lba, const void *buf, unsigned int count);

#endif
