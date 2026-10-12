/* rmdir: ring-3 command.  Removes an empty subdirectory. */

#include "ucmd_lib.h"

void _start(const char *path)
{
	int r;

	if (!path || !path[0]) {
		u_puts("rmdir: usage rmdir /<device>/<dir>\n");
		u_exit(1);
	}

	r = u_syscall1(SYS_RMDIR, (int)path);
	if (r == SYS_ENODEV || r == SYS_ENOFS) {
		u_puts("rmdir: unknown device or invalid path\n");
		u_exit(1);
	}
	if (r == SYS_EIO) {
		u_puts("rmdir: disk error\n");
		u_exit(1);
	}
	if (r == SYS_ENOENT || r == SYS_EINVAL) {
		u_puts("rmdir: invalid path\n");
		u_exit(1);
	}
	if (r == SYS_EROFS) {
		u_puts("rmdir: read-only filesystem\n");
		u_exit(1);
	}
	if (r != 0)
		u_exit(1);   /* the FAT layer printed its own reason */
	u_puts("rmdir ok\n");
	u_exit(0);
}
