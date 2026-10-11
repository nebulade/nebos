/* mkdir: ring-3 command.  Creates a subdirectory. */

#include "ucmd_lib.h"

void _start(const char *path)
{
	int r;

	if (!path || !path[0]) {
		u_puts("mkdir: usage mkdir /<device>/<dir>\n");
		u_exit(1);
	}

	r = u_syscall1(SYS_MKDIR, (int)path);
	if (r == SYS_ENODEV || r == SYS_ENOFS) {
		u_puts("mkdir: unknown device or invalid path\n");
		u_exit(1);
	}
	if (r == SYS_EIO) {
		u_puts("mkdir: disk error\n");
		u_exit(1);
	}
	if (r == SYS_ENOENT || r == SYS_EINVAL) {
		u_puts("mkdir: invalid path\n");
		u_exit(1);
	}
	if (r != 0)
		u_exit(1);   /* the FAT layer printed its own reason */
	u_puts("mkdir ok\n");
	u_exit(0);
}
