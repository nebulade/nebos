/* rm command: delete a file. Usage: rm <path>. */

#include "fat.h"
#include "io.h"

void program_rm_main(const char *arg)
{
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("rm: usage rm <file>\n");
		return;
	}

	r = fat_init();
	if (r == -2) { io_putstr("rm: disk read error\n"); return; }
	if (r != 0) { io_putstr("rm: no FAT filesystem found\n"); return; }

	r = fat_resolve_path(arg, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rm: disk error\n"); return; }
	if (r != 0) { io_putstr("rm: not found\n"); return; }

	r = fat_delete_file_in(dir_cluster, name_8_3);
	if (r == -2) { io_putstr("rm: disk error\n"); return; }
	if (r != 0) { io_putstr("rm: file not found\n"); return; }
	io_putstr("removed\n");
}
