/* rm: ring-3 command.  Deletes a file. */

#include "ucmd_lib.h"

void _start(const char *path)
{
	int r;

	if (!path || !path[0]) {
		u_puts("rm: usage rm /<device>/<file>\n");
		u_exit(1);
	}

	r = u_syscall1(SYS_UNLINK, (int)path);
	if (r == SYS_ENODEV || r == SYS_ENOFS) {
		u_puts("rm: unknown device or invalid path\n");
		u_exit(1);
	}
	if (r == SYS_EIO) {
		u_puts("rm: disk error\n");
		u_exit(1);
	}
	if (r == SYS_ENOENT) {
		u_puts("rm: not found\n");
		u_exit(1);
	}
	if (r == SYS_EROFS) {
		u_puts("rm: read-only filesystem\n");
		u_exit(1);
	}
	if (r != 0) {
		u_puts("rm: file not found\n");
		u_exit(1);
	}
	u_puts("removed\n");
	u_exit(0);
}
