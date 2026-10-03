/* mkdir command: create a subdirectory. Usage: mkdir <path>. */

#include "fat.h"
#include "io.h"

void program_mkdir_main(const char *arg)
{
	unsigned int dir_cluster;
	char name_8_3[12];
	int r;

	if (!arg || !arg[0]) {
		io_putstr("mkdir: usage mkdir <dir>\n");
		return;
	}

	r = fat_init();
	if (r == -2) { io_putstr("mkdir: disk read error\n"); return; }
	if (r != 0) { io_putstr("mkdir: no FAT filesystem found\n"); return; }

	r = fat_resolve_path(arg, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("mkdir: disk error\n"); return; }
	if (r != 0) { io_putstr("mkdir: invalid path\n"); return; }

	r = fat_mkdir(dir_cluster, name_8_3);
	if (r == -2) { io_putstr("mkdir: disk error\n"); return; }
	if (r != 0) return;  /* fat_mkdir already printed */
	io_putstr("mkdir ok\n");
}
