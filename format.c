/* format command: create a fresh FAT filesystem on a device.
 * Usage: format <device> [size]   (size: 100mb, 1gb, ...; binary units) */

#include "blkdev.h"
#include "fatformat.h"
#include "io.h"
#include "process.h"
#include "sched.h"
#include "vga.h"
#include "vfs.h"

static void list_devices(void)
{
	unsigned int i;
	io_putstr("devices:");
	for (i = 0; i < blk_dev_count; i++) {
		io_putchar(' ');
		io_putstr(blk_devs[i].name);
	}
	io_putchar('\n');
}

/* Parse a size such as 100mb or 1gb into 512-byte sectors.  Units are
 * binary (1mb = 1024 * 1024 bytes); a bare number means MiB.  Returns 0,
 * -1 for bad syntax, -2 for too large, -3 for zero. */
static int parse_size(const char *s, unsigned int *out)
{
	unsigned long long v = 0, sectors;
	unsigned int i = 0, shift = 2;   /* MiB */
	int c;

	while (s[i] == ' ')
		i++;
	if (s[i] < '0' || s[i] > '9')
		return -1;
	while (s[i] >= '0' && s[i] <= '9') {
		if (v > 429496729ULL)
			return -2;
		v = v * 10 + (unsigned int)(s[i] - '0');
		i++;
	}
	if (v > 0xFFFFFFFFULL)
		return -2;
	while (s[i] == ' ')
		i++;
	if (s[i]) {
		c = s[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c == 'k')
			shift = 1;
		else if (c == 'm')
			shift = 2;
		else if (c == 'g')
			shift = 3;
		else if (c == 't')
			shift = 4;
		else
			return -1;
		i++;
		c = s[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c == 'b')
			i++;
	}
	while (s[i] == ' ')
		i++;
	if (s[i])
		return -1;
	if (v == 0)
		return -3;
	sectors = v << (shift * 10 - 9);
	if (sectors > 0xFFFFFFFFULL)
		return -2;
	*out = (unsigned int)sectors;
	return 0;
}

/* Show a sector limit in the friendliest exact unit. */
static void print_size(unsigned int sectors)
{
	if (sectors == 0) {
		io_putstr("0 sectors");
	} else if (sectors % 0x80000000u == 0) {   /* 1 TiB */
		format_putdec(sectors >> 31);
		io_putstr(" TiB");
	} else if (sectors % 0x200000u == 0) {     /* 1 GiB */
		format_putdec(sectors >> 21);
		io_putstr(" GiB");
	} else if (sectors % 2048 == 0) {
		format_putdec(sectors / 2048);
		io_putstr(" MiB");
	} else if (sectors % 2 == 0) {
		format_putdec(sectors / 2);
		io_putstr(" KiB");
	} else {
		format_putdec(sectors);
		io_putstr(" sectors");
	}
}

void program_format_main(const char *arg)
{
	blk_dev_t *d;
	char name[16];
	const char *p;
	unsigned int i = 0, j, limit = 0;
	int k, r;

	if (format_busy && process_is_alive(format_owner_pid)) {
		io_putstr("format: another format is in progress\n");
		return;
	}
	format_busy = 0;
	if (!arg) {
		io_putstr("format: usage format <device> [size]\n");
		list_devices();
		return;
	}

	while (arg[i] == ' ')
		i++;
	j = 0;
	while (arg[i] && arg[i] != ' ' && j < sizeof(name) - 1)
		name[j++] = arg[i++];
	name[j] = '\0';

	if (name[0] == '\0') {
		io_putstr("format: usage format <device> [size]\n");
		list_devices();
		return;
	}

	p = arg + i;
	while (*p == ' ')
		p++;
	if (*p) {
		r = parse_size(p, &limit);
		if (r == -1) {
			io_putstr("format: bad size '");
			io_putstr(p);
			io_putstr("' (use e.g. 100mb or 1gb)\n");
			return;
		}
		if (r == -2) {
			io_putstr("format: size is too large\n");
			return;
		}
		if (r == -3) {
			io_putstr("format: size must be greater than zero\n");
			return;
		}
	}

	d = blk_find(name);
	if (!d) {
		io_putstr("format: unknown device '");
		io_putstr(name);
		io_putstr("'\n");
		list_devices();
		return;
	}

	if (d->kind != BLK_FLOPPY && !d->present) {
		io_putstr("format: ");
		io_putstr(d->name);
		io_putstr(": not present\n");
		return;
	}

	if (d->kind == BLK_FLOPPY && limit) {
		io_putstr("format: a floppy is always the full 1440 KiB\n");
		return;
	}

	/* Reject a bad limit before the destructive confirmation prompt. */
	if (d->kind != BLK_FLOPPY && limit) {
		unsigned int avail = (d->total_sectors > 2048)
			? d->total_sectors - 2048 : 0;

		if (limit > avail) {
			io_putstr("format: ");
			print_size(limit);
			io_putstr(" does not fit on ");
			io_putstr(d->name);
			io_putstr(" (");
			print_size(avail);
			io_putstr(" available)\n");
			return;
		}
	}

	io_putstr("format ");
	io_putstr(d->name);
	if (limit) {
		io_putstr(" (");
		print_size(limit);
		io_putstr(")");
	} else {
		io_putstr(" (whole disk)");
	}
	io_putstr("? this destroys all data (y/n) ");
	vga_fb_flush();

	/* Keep the WM from draining the keyboard ring while we poll for the
	 * confirmation (works for PS/2 and USB keyboards alike). */
	io_set_raw_keyboard(1);
	for (;;) {
		k = io_poll_key_with_hw();
		if (k == 'y' || k == 'Y')
			break;
		if (k == 'n' || k == 'N') {
			io_set_raw_keyboard(0);
			io_putstr("aborted\n");
			return;
		}
		/* This kernel command holds the BKL.  Sleep briefly so the
		 * kworker can poll USB HID and the terminal can render the
		 * confirmation prompt; otherwise a USB-only keyboard deadlocks
		 * here. */
		process_sleep_ms(10);
	}
	io_set_raw_keyboard(0);
	io_putchar('\n');

	io_putstr("formatting ");
	io_putstr(d->name);
	io_putstr("...\n");
	vga_fb_flush();

	/* Other filesystem syscalls wait while the FAT is half-written. */
	format_owner_pid = current_proc()->pid;
	format_busy = 1;
	if (fat_format_limited(d, limit) != 0) {
		format_busy = 0;
		io_putstr("format: failed\n");
		return;
	}
	/* Make the drive commit the new metadata before the remount reads it
	 * back (some IDE/SATA bridges acknowledge PIO writes before the data
	 * reaches the medium). */
	{
		int fr;

		bkl_pause();
		fr = blk_flush(d);
		bkl_resume();
		if (fr != 0)
			io_putstr("format: cache flush failed; remount may be stale\n");
	}

	/* Remount only the formatted device; a full vfs_mount_all() would probe
	 * every disk again and was a second large stall. */
	vfs_remount_device(d);
	format_busy = 0;

	io_putstr("done\n");
}
