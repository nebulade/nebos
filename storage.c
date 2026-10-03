/* storage command: probe floppy + IDE devices and report FAT filesystems. */

#include "blkdev.h"
#include "fatdetect.h"
#include "io.h"

static void put_dec(unsigned int n)
{
	char buf[12];
	unsigned int i = sizeof(buf) - 1;
	buf[i] = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i > 0) {
		buf[--i] = (char)('0' + (n % 10));
		n /= 10;
	}
	io_putstr(buf + i);
}

static void put_size(unsigned int bytes)
{
	if (bytes >= 1024u * 1024u * 1024u) {
		unsigned int gb = bytes / (1024u * 1024u * 1024u);
		unsigned int tenths = (bytes % (1024u * 1024u * 1024u)) * 10u / (1024u * 1024u * 1024u);
		put_dec(gb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" GB");
	} else if (bytes >= 1024u * 1024u) {
		unsigned int mb = bytes / (1024u * 1024u);
		unsigned int tenths = (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u);
		put_dec(mb);
		if (tenths) {
			io_putchar('.');
			put_dec(tenths);
		}
		io_putstr(" MB");
	} else {
		put_dec(bytes / 1024u);
		io_putstr(" KB");
	}
}

static const char *fat_name(fat_type_t t)
{
	switch (t) {
	case FAT12: return "FAT12";
	case FAT16: return "FAT16";
	case FAT32: return "FAT32";
	default:    return "FAT";
	}
}

static void report_fs(const char *name, const fat_info_t *info)
{
	io_putstr(name);
	io_putstr(": ");
	io_putstr(fat_name(info->type));
	io_putchar(' ');
	put_size(info->total_sectors * info->bytes_per_sector);
	if (info->label[0]) {
		io_putstr("  \"");
		io_putstr(info->label);
		io_putchar('"');
	}
	io_putchar('\n');
}

static void part_name(char *out, const char *base, unsigned int idx)
{
	unsigned int i = 0;
	while (base[i] && i < 14) {
		out[i] = base[i];
		i++;
	}
	out[i++] = 'p';
	out[i++] = (char)('0' + idx + 1);
	out[i] = '\0';
}

void program_storage_main(void)
{
	static unsigned char buf[512];
	static unsigned char pbuf[512];
	unsigned int i;

	blk_scan();

	for (i = 0; i < blk_dev_count; i++) {
		blk_dev_t *d = &blk_devs[i];
		fat_info_t info;

		if (d->kind == BLK_IDE && !d->present) {
			io_putstr(d->name);
			io_putstr(": not present\n");
			continue;
		}

		if (blk_read(d, 0, buf, 1) != 0) {
			io_putstr(d->name);
			io_putstr(": not present / unreadable\n");
			continue;
		}

		if (d->kind == BLK_FLOPPY) {
			if (fat_detect(buf, &info) == 0)
				report_fs(d->name, &info);
			else {
				io_putstr(d->name);
				io_putstr(": no valid FAT filesystem\n");
			}
		} else {
			mbr_part_t parts[4];
			int n = mbr_parse(buf, parts);
			int found = 0;
			int j;

			for (j = 0; j < n; j++) {
				char pname[16];
				if (!parts[j].is_fat || parts[j].start_lba == 0 ||
				    parts[j].sectors == 0)
					continue;
				if (blk_read(d, parts[j].start_lba, pbuf, 1) != 0)
					continue;
				if (fat_detect(pbuf, &info) == 0) {
					part_name(pname, d->name, parts[j].index);
					report_fs(pname, &info);
					found = 1;
				}
			}

			if (!found) {
				if (fat_detect(buf, &info) == 0)
					report_fs(d->name, &info);
				else {
					io_putstr(d->name);
					io_putstr(": no valid FAT filesystem\n");
				}
			}
		}
	}
}
