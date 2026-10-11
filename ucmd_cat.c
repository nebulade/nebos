/* cat: ring-3 command.  Prints a file's contents. */

#include "ucmd_lib.h"

#define DUMP_BUF_SIZE 4096

static unsigned char buf[DUMP_BUF_SIZE];

void _start(const char *path)
{
	int n;
	unsigned int i;

	if (!path || !path[0]) {
		u_puts("cat: usage cat /<device>/<file>\n");
		u_exit(1);
	}

	n = u_syscall3(SYS_READ_FILE, (int)path, (int)buf, DUMP_BUF_SIZE);
	if (n == SYS_ENODEV) {
		u_puts("cat: no such device\n");
		u_exit(1);
	}
	if (n == SYS_ENOFS) {
		u_puts("cat: no filesystem on device\n");
		u_exit(1);
	}
	if (n == SYS_EIO) {
		u_puts("cat: disk read error\n");
		u_exit(1);
	}
	if (n < 0) {
		u_puts("cat: not found\n");
		u_exit(1);
	}

	for (i = 0; i < (unsigned int)n; i++) {
		unsigned char c = buf[i];

		if (c >= 0x20 && c < 0x7F)
			u_putc((char)c);
		else if (c == '\n')
			u_putc('\n');
		else if (c == '\t')
			u_putc('\t');
		else
			u_putc('.');
	}
	if (n > 0 && buf[n - 1] != '\n')
		u_puts("\n");
	u_exit(0);
}
