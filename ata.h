/* ATA PIO driver (polling, no IRQ). Primary 0x1F0 / secondary 0x170. */
#ifndef ATA_H
#define ATA_H

/* Probe an ATA device (bus 0=primary,1=secondary; unit 0=master,1=slave).
 * Returns 0 and fills total_sectors/model if an ATA disk is present, else -1. */
int ata_probe(int bus, int unit, unsigned int *total_sectors, char model[41]);

/* Read/write sectors via ATA PIO. Return 0 on success, -1 on error. */
int ata_read_sectors(int bus, int unit, unsigned int lba, void *buf, unsigned int count);
int ata_write_sectors(int bus, int unit, unsigned int lba, const void *buf, unsigned int count);

#endif
