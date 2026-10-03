/* cat command: print file contents to screen (supports paths). */

#include "fat.h"
#include "io.h"

#define DUMP_BUF_SIZE 4096

void program_cat_main(const char *filename)
{
	static unsigned char buf[DUMP_BUF_SIZE];
	unsigned int dir_cluster;
	char name_8_3[12];
	int n;
	unsigned int i;

	if (!filename || !filename[0]) {
		io_putstr("cat: usage cat <file>\n");
		return;
	}

	n = fat_init();
	if (n == -2) {
		io_putstr("cat: disk read error\n");
		return;
	}
	if (n != 0) {
		io_putstr("cat: no FAT filesystem found\n");
		return;
	}

	n = fat_resolve_path(filename, &dir_cluster, name_8_3);
	if (n == -2) {
		io_putstr("cat: disk error\n");
		return;
	}
	if (n != 0) {
		io_putstr("cat: not found\n");
		return;
	}

	n = fat_read_file_in(dir_cluster, name_8_3, buf, DUMP_BUF_SIZE);
	if (n == -2) {
		io_putstr("cat: disk read error\n");
		return;
	}
	if (n < 0) {
		io_putstr("cat: file not found\n");
		return;
	}

	for (i = 0; i < (unsigned int)n; i++) {
		unsigned char c = buf[i];
		if (c >= 0x20 && c < 0x7F)
			io_putchar((char)c);
		else if (c == '\n')
			io_putchar('\n');
		else if (c == '\t')
			io_putchar('\t');
		else
			io_putchar('.');
	}
	if (n > 0 && buf[n - 1] != '\n')
		io_putstr("\n");
}
