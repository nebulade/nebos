/* save command: write a text string to a file.
 * Usage: save /<device>/<file> <text...>. */

#include "fat.h"
#include "io.h"
#include "vfs.h"

void program_save_main(const char *arg)
{
	char fn[64];
	unsigned int i, fn_len, text_len;
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	const char *text;
	int r;

	if (!arg || !arg[0]) {
		io_putstr("save: usage save /<device>/<file> <text>\n");
		return;
	}

	/* filename = first token */
	i = 0;
	while (arg[i] == ' ')
		i++;
	fn_len = 0;
	while (arg[i + fn_len] && arg[i + fn_len] != ' ' && fn_len < sizeof(fn) - 1)
		fn_len++;
	{
		unsigned int j;
		for (j = 0; j < fn_len; j++)
			fn[j] = arg[i + j];
	}
	fn[fn_len] = '\0';
	if (fn_len == 0) {
		io_putstr("save: usage save /<device>/<file> <text>\n");
		return;
	}

	/* text = the rest */
	i += fn_len;
	while (arg[i] == ' ')
		i++;
	text = arg + i;
	text_len = 0;
	while (text[text_len])
		text_len++;

	if (vfs_resolve(fn, &fs, &fs_path) != 0) {
		io_putstr("save: read-only filesystem\n");
		return;
	}

	r = fat_resolve_path(fs, fs_path, &dir_cluster, name_8_3);
	if (r == -2) { io_putstr("save: disk error\n"); return; }
	if (r != 0) { io_putstr("save: invalid path\n"); return; }

	r = fat_write_file_in(fs, dir_cluster, name_8_3, text, text_len);
	if (r == -2) { io_putstr("save: disk write error\n"); return; }
	if (r != 0) { io_putstr("save: write failed\n"); return; }
	io_putstr("saved\n");
}
