/* mkdir command: create a subdirectory. Usage: mkdir /<device>/<dir>. */

#include "fat.h"
#include "io.h"
#include "vfs.h"

void program_mkdir_main(const char *arg)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("mkdir: usage mkdir /<device>/<dir>\n");
		return;
	}

	if (vfs_resolve(arg, &fs, &fs_path) != 0) {
		io_putstr("mkdir: unknown device or invalid path\n");
		return;
	}

	r = fat_resolve_path(fs, fs_path, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("mkdir: disk error\n"); return; }
	if (r != 0) { io_putstr("mkdir: invalid path\n"); return; }

	r = fat_mkdir(fs, dir_cluster, name_8_3);
	if (r == -2) { io_putstr("mkdir: disk error\n"); return; }
	if (r != 0) return;  /* fat_mkdir already printed */
	io_putstr("mkdir ok\n");
}
