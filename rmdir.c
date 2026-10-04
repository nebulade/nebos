/* rmdir command: remove an empty subdirectory. Usage: rmdir /<device>/<dir>. */

#include "fat.h"
#include "io.h"
#include "vfs.h"

void program_rmdir_main(const char *arg)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("rmdir: usage rmdir /<device>/<dir>\n");
		return;
	}

	vfs_mount_all();
	if (vfs_resolve(arg, &fs, &fs_path) != 0) {
		io_putstr("rmdir: unknown device or invalid path\n");
		return;
	}

	r = fat_resolve_path(fs, fs_path, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rmdir: disk error\n"); return; }
	if (r != 0) { io_putstr("rmdir: invalid path\n"); return; }

	r = fat_rmdir(fs, dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rmdir: disk error\n"); return; }
	if (r != 0) return;  /* fat_rmdir already printed */
	io_putstr("rmdir ok\n");
}
