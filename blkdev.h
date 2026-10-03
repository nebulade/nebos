/* Block device registry: floppy + IDE. Shared by the `storage` and `format`
 * commands so they agree on device names and read/write paths. */
#ifndef BLKDEV_H
#define BLKDEV_H

typedef enum { BLK_FLOPPY, BLK_IDE } blk_kind_t;

typedef struct {
	const char *name;          /* "floppy0","floppy1","ide0".."ide3" */
	blk_kind_t kind;
	unsigned int bus;          /* floppy: drive #; ide: 0=primary,1=secondary */
	unsigned int unit;         /* ide: 0=master,1=slave */
	int present;               /* IDE: set by IDENTIFY; floppy: always listed */
	unsigned int total_sectors;/* 0 if unknown (floppy) */
	char model[41];            /* IDE model string */
} blk_dev_t;

#define BLKDEV_MAX 8
extern blk_dev_t blk_devs[BLKDEV_MAX];
extern unsigned int blk_dev_count;

/* Probe devices and populate the registry. */
void blk_scan(void);

/* Find a device by name (NULL if not found). */
blk_dev_t *blk_find(const char *name);

/* Read/write count sectors at lba. Return 0 on success, -1 on error. */
int blk_read(blk_dev_t *d, unsigned int lba, void *buf, unsigned int count);
int blk_write(blk_dev_t *d, unsigned int lba, const void *buf, unsigned int count);

#endif
