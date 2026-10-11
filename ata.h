/* ATA PIO driver (polling, no IRQ). Primary 0x1F0 / secondary 0x170. */
#ifndef ATA_H
#define ATA_H

/* Probe an ATA device (bus 0=primary,1=secondary; unit 0=master,1=slave).
 * Returns 0 and fills total_sectors/model if an ATA disk is present, else -1. */
int ata_probe(int bus, int unit, unsigned int *total_sectors, char model[41]);

/* Configure the channel port bases.  Scans for a PCI IDE interface
 * (class 01:01): in native mode (prog-if bit 0/2) the command/control blocks
 * live in BAR0..BAR3, otherwise the fixed legacy ports are used.  Idempotent;
 * call before ata_probe(). */
void ata_init_ports(void);

/* Parse an IDENTIFY DEVICE response (256 words): fills total_sectors (clamped
 * to 32 bits) and a trimmed 40-char model string. Returns 0 for an ATA disk,
 * -1 for a non-disk (e.g. ATAPI). Shared by the PATA and AHCI drivers. */
int ata_identify_parse(const unsigned short *id, unsigned int *total_sectors, char model[41]);

/* Read/write sectors via ATA PIO. Return 0 on success, -1 on error. */
int ata_read_sectors(int bus, int unit, unsigned int lba, void *buf, unsigned int count);
int ata_write_sectors(int bus, int unit, unsigned int lba, const void *buf, unsigned int count);

#endif
