/* save: ring-3 command.  Writes a text string to a file.
 * The terminal resolves the path and passes "<absolute-path> <text...>" as the
 * single argument, so this only splits it and calls the kernel service. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	char fn[64];
	unsigned int i = 0, fn_len = 0, text_len = 0;
	const char *text;
	int r;

	if (!arg || !arg[0]) {
		u_puts("save: usage save /<device>/<file> <text>\n");
		u_exit(1);
	}

	while (arg[i] == ' ')
		i++;
	while (arg[i + fn_len] && arg[i + fn_len] != ' ' && fn_len < sizeof(fn) - 1)
		fn_len++;
	{
		unsigned int j;
		for (j = 0; j < fn_len; j++)
			fn[j] = arg[i + j];
	}
	fn[fn_len] = '\0';
	if (fn_len == 0) {
		u_puts("save: usage save /<device>/<file> <text>\n");
		u_exit(1);
	}

	i += fn_len;
	while (arg[i] == ' ')
		i++;
	text = arg + i;
	while (text[text_len])
		text_len++;

	r = u_syscall3(SYS_WRITE_FILE, (int)fn, (int)text, (int)text_len);
	if (r == SYS_ENODEV || r == SYS_ENOFS) {
		u_puts("save: unknown device or invalid path\n");
		u_exit(1);
	}
	if (r == SYS_EIO) {
		u_puts("save: disk write error\n");
		u_exit(1);
	}
	if (r != 0) {
		u_puts("save: write failed\n");
		u_exit(1);
	}
	u_puts("saved\n");
	u_exit(0);
}
