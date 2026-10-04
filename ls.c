/* ls command: list a directory. `ls` / `ls /` lists mounted filesystems;
 * `ls /<device>[/dir]` lists a directory on that filesystem. */

#include "fat.h"
#include "io.h"
#include "vfs.h"

#define BUF_SIZE 12

static void list_cb(const char *name, int is_dir, unsigned int size)
{
	unsigned int i;
	char buf[BUF_SIZE];
	unsigned int n;

	for (i = 0; i < 11 && name[i]; i++)
		io_putchar(name[i] == ' ' ? ' ' : name[i]);
	if (i < 11)
		io_putchar(' ');
	if (is_dir) {
		io_putstr("  <DIR>\n");
		return;
	}
	io_putstr(" ");
	n = size;
	if (n == 0) {
		io_putchar('0');
	} else {
		buf[0] = '\0';
		i = BUF_SIZE - 1;
		buf[i] = '\0';
		while (n && i > 0) {
			buf[--i] = (char)('0' + (n % 10));
			n /= 10;
		}
		io_putstr(buf + i);
	}
	io_putstr("\n");
}

void program_ls_main(const char *path)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	int r;

	vfs_mount_all();

	if (!path || !path[0] || (path[0] == '/' && path[1] == '\0')) {
		vfs_list_root();
		return;
	}

	if (vfs_resolve(path, &fs, &fs_path) != 0) {
		if (vfs_device_known(path))
			io_putstr("ls: no filesystem on device\n");
		else
			io_putstr("ls: no such device\n");
		return;
	}

	r = fat_resolve_dir(fs, fs_path, &dir_cluster);
	if (r == -2) {
		io_putstr("ls: disk error\n");
		return;
	}
	if (r != 0) {
		io_putstr("ls: not a directory\n");
		return;
	}

	r = fat_list_dir(fs, dir_cluster, list_cb);
	if (r == -2)
		io_putstr("ls: disk read error\n");
}
