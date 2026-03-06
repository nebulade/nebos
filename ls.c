/* ls command: list root directory of FAT12 floppy. */

#include "fat.h"
#include "io.h"

#define BUF_SIZE 12

static void list_cb(const char *name, int is_dir, unsigned int size)
{
	unsigned int i;
	char buf[BUF_SIZE];
	unsigned int n;

	(void)is_dir;
	for (i = 0; i < 11 && name[i]; i++)
		io_putchar(name[i] == ' ' ? ' ' : name[i]);
	if (i < 11)
		io_putchar(' ');
	io_putstr(" ");
	/* Simple decimal print for size */
	n = size;
	if (n == 0) {
		io_putchar('0');
	} else {
		buf[0] = '\0';
		i = BUF_SIZE - 1;
		buf[i] = '\0';
		while (n && i > 0) {
			buf[--i] = (char)('0' + (n % 10));
			n /= 10;
		}
		io_putstr(buf + i);
	}
	io_putstr("\n");
}

void program_ls_main(void)
{
	int r;

	if (fat_init() != 0) {
		io_putstr("ls: fat init failed\n");
		return;
	}
	r = fat_list_root(list_cb);
	if (r == -2) {
		io_putstr("ls: disk read error\n");
		return;
	}
	if (r != 0) {
		io_putstr("ls: list failed\n");
		return;
	}
}
