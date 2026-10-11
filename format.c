/* format command: create a fresh FAT filesystem on a device. Usage: format <device>. */

#include "blkdev.h"
#include "fatformat.h"
#include "io.h"
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

void program_format_main(const char *arg)
{
	blk_dev_t *d;
	char name[16];
	unsigned int i = 0, j;
	int k;

	if (!arg) {
		io_putstr("format: usage format <device>\n");
		blk_scan();
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
		io_putstr("format: usage format <device>\n");
		blk_scan();
		list_devices();
		return;
	}

	blk_scan();

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

	io_putstr("format ");
	io_putstr(d->name);
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
	}
	io_set_raw_keyboard(0);
	io_putchar('\n');

	io_putstr("formatting ");
	io_putstr(d->name);
	io_putstr("...\n");
	vga_fb_flush();

	if (fat_format(d) != 0) {
		io_putstr("format: failed\n");
		return;
	}

	/* Make the fresh filesystem immediately usable (save/ls/cat). */
	vfs_mount_all();

	io_putstr("done\n");
}
