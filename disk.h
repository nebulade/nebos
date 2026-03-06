/* Disk layer: read sectors via direct FDC + ISA DMA in protected mode. */

#ifndef DISK_H
#define DISK_H

/* Select the floppy drive (0 or 1) used by subsequent disk_read_sectors calls.
 * Resets FDC state so the next read re-initialises for the new drive. */
void disk_set_drive(unsigned int drive);

/* Return currently selected drive number. */
unsigned int disk_get_drive(void);

/* Read count sectors starting at lba into buf.
 * Returns 0 on success, -1 on error. */
int disk_read_sectors(unsigned int lba, void *buf, unsigned int count);

#endif
