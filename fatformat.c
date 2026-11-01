/* Create a fresh FAT filesystem on a block device.
 * Floppy: FAT12 superfloppy (no MBR). IDE/SATA: MBR + single FAT16/32 partition. */

#include "fatformat.h"
#include "io.h"
#include "mm.h"
#include "sched.h"

#define SEC_SIZE 512

/* Zero-fill chunk size for the FAT/reserved/root areas.  Large disks would
 * otherwise need millions of single-sector writes (a FAT32 on a 1 TB disk has
 * ~2 M FAT sectors). */
#define ZERO_CHUNK_PAGES 32

volatile int format_busy;
volatile unsigned int format_owner_pid;

static unsigned int fmt_total, fmt_done, fmt_last_pct;

static void format_pct(unsigned int pct)
{
	char b[4];
	unsigned int n = 0;

	b[n++] = (char)('0' + pct / 100);
	b[n++] = (char)('0' + (pct / 10) % 10);
	b[n++] = (char)('0' + pct % 10);
	io_putstr("format: ");
	{ unsigned int i; for (i = 0; i < n; i++) io_putchar(b[i]); }
	io_putstr("%\n");
}

static void format_progress(unsigned int sectors)
{
	unsigned int pct;

	if (!fmt_total)
		return;
	fmt_done += sectors;
	pct = (fmt_done * 100u) / fmt_total;
	if (pct > 100) pct = 100;
	if (pct >= fmt_last_pct + 10 || pct == 100) {
		fmt_last_pct = pct;
		format_pct(pct);
	}
}

/* A chunk write releases the BKL so the WM/netlog/terminal can run between
 * disk transfers; format_busy keeps other filesystem syscalls out. */
static int format_write(blk_dev_t *d, unsigned int lba, const void *buf,
			unsigned int count)
{
	int r;

	bkl_pause();
	r = blk_write(d, lba, buf, count);
	bkl_resume();
	return r;
}

/* Write `count` zero sectors at `lba` in `chunk`-sector pieces. */
static int write_zeros(blk_dev_t *d, unsigned int lba, unsigned int count,
		       const unsigned char *buf, unsigned int chunk)
{
	unsigned int done = 0;

	while (done < count) {
		unsigned int n = count - done;
		if (n > chunk)
			n = chunk;
		if (format_write(d, lba + done, buf, n) != 0)
			return -1;
		format_progress(n);
		done += n;
	}
	return 0;
}

static void put16(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void zero_sector(unsigned char *s)
{
	unsigned int i;
	for (i = 0; i < SEC_SIZE; i++)
		s[i] = 0;
}

static unsigned int fat_sec_per_fat(unsigned int type, unsigned int clusters)
{
	unsigned int entries = clusters + 2;
	if (type == 12)
		return (entries * 3 + 1023) / 1024;
	if (type == 16)
		return (entries * 2 + 511) / 512;
	return (entries * 4 + 511) / 512;
}

static void choose_layout(unsigned int fs_sectors, unsigned int *type,
			  unsigned int *spc, unsigned int *root_entries)
{
	if (fs_sectors < 4085) {
		*type = 12;
		*spc = 1;
		*root_entries = (fs_sectors <= 2880) ? 224 : 512;
	} else if (fs_sectors <= 4193536u) {   /* ~2 GB */
		*type = 16;
		*spc = 1;
		while (fs_sectors / *spc >= 65525u)
			*spc <<= 1;
		*root_entries = 512;
	} else {
		*type = 32;
		*spc = 8;
		while (fs_sectors / *spc >= 268435445u)
			*spc <<= 1;
		*root_entries = 0;
	}
}

static void build_boot_sector(unsigned char *s, unsigned int type, unsigned int spc,
			      unsigned int reserved, unsigned int num_fats,
			      unsigned int root_entries, unsigned int sec_per_fat,
			      unsigned int fs_sectors, unsigned int hidden,
			      unsigned int media)
{
	unsigned int i;
	zero_sector(s);

	s[0] = 0xEB;
	s[1] = (type == 32) ? 0x58 : 0x3C;
	s[2] = 0x90;
	s[3] = 'M'; s[4] = 'S'; s[5] = 'D'; s[6] = 'O'; s[7] = 'S';
	s[8] = '5'; s[9] = '.'; s[10] = '0';

	put16(s + 0x0B, 512);
	s[0x0D] = (unsigned char)spc;
	put16(s + 0x0E, reserved);
	s[0x10] = (unsigned char)num_fats;
	put16(s + 0x11, root_entries);
	put16(s + 0x13, (fs_sectors < 0x10000) ? fs_sectors : 0);
	s[0x15] = (unsigned char)media;
	put16(s + 0x16, (type != 32) ? sec_per_fat : 0);
	put16(s + 0x18, (hidden == 0) ? 18 : 63);   /* sectors per track */
	put16(s + 0x1A, (hidden == 0) ? 2 : 255);   /* heads */
	put32(s + 0x1C, hidden);
	put32(s + 0x20, fs_sectors);

	if (type == 32) {
		put32(s + 0x24, sec_per_fat);
		s[0x28] = 0; s[0x29] = 0;               /* ext flags */
		s[0x2A] = 0; s[0x2B] = 0;               /* fs version */
		put32(s + 0x2C, 2);                     /* root cluster */
		put16(s + 0x30, 1);                     /* FSInfo sector */
		put16(s + 0x32, 6);                     /* backup boot sector */
		s[0x40] = 0x80;                         /* drive number */
		s[0x42] = 0x29;                         /* boot signature */
		put32(s + 0x43, 0x12345678);            /* volume id */
		for (i = 0; i < 11; i++) s[0x47 + i] = ' ';
		s[0x52] = 'F'; s[0x53] = 'A'; s[0x54] = 'T'; s[0x55] = '3';
		s[0x56] = '2'; s[0x57] = ' '; s[0x58] = ' '; s[0x59] = ' ';
	} else {
		s[0x24] = (hidden == 0) ? 0x00 : 0x80;  /* drive number */
		s[0x26] = 0x29;                         /* boot signature */
		put32(s + 0x27, 0x12345678);            /* volume id */
		for (i = 0; i < 11; i++) s[0x2B + i] = ' ';
		s[0x36] = 'F'; s[0x37] = 'A'; s[0x38] = 'T'; s[0x39] = '1';
		s[0x3A] = (type == 12) ? '2' : '6';
		s[0x3B] = ' '; s[0x3C] = ' '; s[0x3D] = ' ';
	}

	s[510] = 0x55;
	s[511] = 0xAA;
}

static void build_fat_first_sector(unsigned char *s, unsigned int type,
				   unsigned int media)
{
	zero_sector(s);
	s[0] = (unsigned char)media;
	if (type == 12) {
		s[1] = 0xFF;
		s[2] = 0xFF;
	} else if (type == 16) {
		s[1] = 0xFF;
		s[2] = 0xFF;
		s[3] = 0xFF;
	} else { /* FAT32 */
		s[1] = 0xFF; s[2] = 0xFF; s[3] = 0x0F;   /* entry 0 */
		s[4] = 0xFF; s[5] = 0xFF; s[6] = 0xFF; s[7] = 0x0F;   /* entry 1 */
		s[8] = 0xFF; s[9] = 0xFF; s[10] = 0xFF; s[11] = 0x0F;  /* entry 2 (root) */
	}
}

static void build_fsinfo_sector(unsigned char *s)
{
	zero_sector(s);
	put32(s + 0x000, 0x41615252u);   /* LeadSig "RRaA" */
	put32(s + 0x1E4, 0x61417272u);   /* StrucSig "rrAa" */
	put32(s + 0x1E8, 0xFFFFFFFFu);   /* free cluster count: unknown */
	put32(s + 0x1EC, 0xFFFFFFFFu);   /* next free cluster: unknown */
	put32(s + 0x1FC, 0xAA550000u);   /* TrailSig */
}

int fat_format(blk_dev_t *d)
{
	static unsigned char sec[SEC_SIZE];
	static unsigned char fat_sec[SEC_SIZE];
	static unsigned char zero[SEC_SIZE];
	unsigned char *zbuf;
	unsigned int zchunk, zpages;
	unsigned int fs_sectors, part_start;
	unsigned int type, spc, root_entries;
	unsigned int reserved, num_fats = 2, sec_per_fat, root_dir_sectors;
	unsigned int clusters, fat_start, root_start, data_start;
	unsigned int media;
	int is_floppy = (d->kind == BLK_FLOPPY);
	unsigned int i;

	if (!d)
		return -1;

	if (is_floppy) {
		fs_sectors = 2880;
		part_start = 0;
		media = 0xF0;
	} else {
		if (d->total_sectors < (2048 + 64))
			return -1;   /* too small to partition */
		part_start = 2048;
		fs_sectors = d->total_sectors - part_start;
		media = 0xF8;
	}

	choose_layout(fs_sectors, &type, &spc, &root_entries);
	reserved = (type == 32) ? 32 : 1;

	/* Converge on FAT size + cluster count. */
	clusters = fs_sectors / spc;
	for (i = 0; i < 3; i++) {
		unsigned int data;
		sec_per_fat = fat_sec_per_fat(type, clusters);
		data = fs_sectors - reserved - num_fats * sec_per_fat
			- ((root_entries * 32 + 511) / 512);
		clusters = data / spc;
	}
	sec_per_fat = fat_sec_per_fat(type, clusters);
	root_dir_sectors = (root_entries * 32 + 511) / 512;
	fat_start = reserved;
	root_start = reserved + num_fats * sec_per_fat;
	data_start = root_start + root_dir_sectors;

	fmt_total = reserved + num_fats * sec_per_fat + root_dir_sectors;
	if (type == 32)
		fmt_total += spc;
	fmt_done = 0;
	fmt_last_pct = 0;

	zero_sector(zero);

	/* Bulk zero buffer (a 1 TB FAT32 has ~2 M FAT sectors; single-sector
	 * writes would take minutes).  Real disks only -- floppies keep the
	 * 512-byte path. */
	zbuf = zero;
	zchunk = 1;
	zpages = 0;
	if (!is_floppy) {
		zbuf = (unsigned char *)mm_alloc_pages(ZERO_CHUNK_PAGES);
		if (zbuf) {
			unsigned int k;
			for (k = 0; k < ZERO_CHUNK_PAGES * PAGE_SIZE; k++)
				zbuf[k] = 0;
			zchunk = ZERO_CHUNK_PAGES * (PAGE_SIZE / SEC_SIZE);
			zpages = ZERO_CHUNK_PAGES;
		} else {
			zbuf = zero;
			zchunk = 1;
		}
	}

	/* 1. MBR (IDE/SATA only). */
	if (!is_floppy) {
		unsigned char *e;
		zero_sector(sec);
		e = sec + 0x1BE;
		e[0] = 0x80;                            /* bootable */
		e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;   /* start CHS (C0 H0 S2) */
		e[4] = (type == 32) ? 0x0C : (type == 16 ? 0x0E : 0x01);
		e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;   /* end CHS (max, LBA) */
		put32(e + 8, part_start);
		put32(e + 12, fs_sectors);
		sec[510] = 0x55;
		sec[511] = 0xAA;
		if (format_write(d, 0, sec, 1) != 0)
			return -1;
	}

	/* 2. Reserved area + boot sector (+ FSInfo/backup for FAT32). */
	build_boot_sector(sec, type, spc, reserved, num_fats, root_entries,
			  sec_per_fat, fs_sectors, part_start, media);
	if (write_zeros(d, part_start, reserved, zbuf, zchunk) != 0)
		return -1;
	if (format_write(d, part_start, sec, 1) != 0)
		return -1;
	if (type == 32) {
		if (format_write(d, part_start + 6, sec, 1) != 0)   /* backup boot */
			return -1;
		build_fsinfo_sector(sec);
		if (format_write(d, part_start + 1, sec, 1) != 0)   /* FSInfo */
			return -1;
	}

	/* 3. FATs (two copies).  The first sector carries the media byte and
	 * the end-of-chain markers; the rest must be zero. */
	build_fat_first_sector(fat_sec, type, media);
	for (i = 0; i < num_fats; i++) {
		unsigned int base = part_start + fat_start + i * sec_per_fat;
		if (format_write(d, base, fat_sec, 1) != 0)
			return -1;
		if (sec_per_fat > 1 &&
		    write_zeros(d, base + 1, sec_per_fat - 1, zbuf, zchunk) != 0)
			return -1;
	}

	/* 4. Root directory (FAT12/16) or root cluster (FAT32). */
	if (type == 32) {
		if (write_zeros(d, part_start + data_start, spc, zbuf, zchunk) != 0)
			return -1;
	} else {
		if (write_zeros(d, part_start + root_start, root_dir_sectors,
				zbuf, zchunk) != 0)
			return -1;
	}

	if (zpages) {
		unsigned int k;
		for (k = 0; k < zpages; k++)
			mm_free_page((unsigned int)zbuf + k * PAGE_SIZE);
	}

	return 0;
}
