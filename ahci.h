/* AHCI (SATA) driver: polled, no IRQs, no NCQ.
 * Finds the first AHCI controller (PCI class 01:06:01), brings its ports up
 * and exposes the attached ATA disks. Read/write use DMA (READ/WRITE DMA EXT,
 * LBA48) through a bounce buffer. */
#ifndef AHCI_H
#define AHCI_H

#define AHCI_MAX_DISKS 8

struct ahci_disk {
	unsigned int port;          /* controller port number */
	unsigned int sectors;       /* total sectors (clamped to 32 bits) */
	char model[41];
};

/* Initialise the first AHCI controller. Idempotent; returns 0 if a controller
 * was found and brought up, -1 otherwise. */
int ahci_init(void);

/* 1 if an AHCI controller was found and initialised. */
int ahci_controller_present(void);

/* Collect present ATA disks. Returns the number written (<= max). */
unsigned int ahci_scan(struct ahci_disk *out, unsigned int max);

/* Read/write `count` sectors at LBA `lba` on the given controller port.
 * Return 0 on success, -1 on error. */
int ahci_read_sectors(unsigned int port, unsigned int lba, void *buf, unsigned int count);
int ahci_write_sectors(unsigned int port, unsigned int lba, const void *buf, unsigned int count);

/* Flush the port's write cache (FLUSH CACHE EXT). */
int ahci_flush(unsigned int port);

#endif
