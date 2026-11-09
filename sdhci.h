/* Polled SDHCI/eMMC driver for the Intel Braswell MMC controller. */
#ifndef SDHCI_H
#define SDHCI_H

struct sdhci_disk {
	unsigned int sectors;      /* 512-byte sectors */
	char model[41];
};

/* Probe the Intel MMC controller and initialise the eMMC.  Returns the
 * number of disks found (0 or 1); the result is cached after the first
 * call.  Logs the PCI id, BAR, capabilities and eMMC capacity to COM1. */
unsigned int sdhci_scan(struct sdhci_disk *out, unsigned int max);

/* 512-byte-sector I/O on the selected eMMC.  Returns 0 on success. */
int sdhci_read_sectors(unsigned int lba, void *buf, unsigned int count);
int sdhci_write_sectors(unsigned int lba, const void *buf, unsigned int count);

/* The eMMC write path completes on the write command; there is no host
 * cache to flush. */
int sdhci_flush(void);

#endif
