/* rm command: delete a file. Usage: rm /<device>/<file>. */

#include "fat.h"
#include "io.h"
#include "vfs.h"

void program_rm_main(const char *arg)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("rm: usage rm /<device>/<file>\n");
		return;
	}

	if (vfs_resolve(arg, &fs, &fs_path) != 0) {
		io_putstr("rm: read-only filesystem\n");
		return;
	}

	r = fat_resolve_path(fs, fs_path, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rm: disk error\n"); return; }
	if (r != 0) { io_putstr("rm: not found\n"); return; }

	r = fat_delete_file_in(fs, dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rm: disk error\n"); return; }
	if (r != 0) { io_putstr("rm: file not found\n"); return; }
	io_putstr("removed\n");
}
