/* rmdir command: remove an empty subdirectory. Usage: rmdir <path>. */

#include "fat.h"
#include "io.h"

void program_rmdir_main(const char *arg)
{
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("rmdir: usage rmdir <dir>\n");
		return;
	}

	r = fat_init();
	if (r == -2) { io_putstr("rmdir: disk read error\n"); return; }
	if (r != 0) { io_putstr("rmdir: no FAT filesystem found\n"); return; }

	r = fat_resolve_path(arg, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rmdir: disk error\n"); return; }
	if (r != 0) { io_putstr("rmdir: invalid path\n"); return; }

	r = fat_rmdir(dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rmdir: disk error\n"); return; }
	if (r != 0) return;  /* fat_rmdir already printed */
	io_putstr("rmdir ok\n");
}
