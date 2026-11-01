/* ls: ring-3 command. `ls` / `ls /` lists mounted filesystems;
 * `ls /<device>[/dir]` lists a directory on that filesystem. */

#include "ucmd_lib.h"

#define VFS_PATH_MAX 64

#define BUF_SIZE 12

static void put_size_sectors(unsigned int sectors)
{
	if (sectors >= 2u * 1024u * 1024u) {           /* >= 1 GiB */
		unsigned int gb = sectors >> 21;
		unsigned int tenths = ((sectors & 0x1FFFFFu) * 10u) >> 21;
		u_putdec(gb);
		if (tenths) {
			u_putc('.');
			u_putdec(tenths);
		}
		u_puts(" GB");
	} else if (sectors >= 2048u) {                 /* >= 1 MiB */
		unsigned int mb = sectors >> 11;
		unsigned int tenths = ((sectors & 0x7FFu) * 10u) >> 11;
		u_putdec(mb);
		if (tenths) {
			u_putc('.');
			u_putdec(tenths);
		}
		u_puts(" MB");
	} else {
		u_putdec(sectors / 2u);
		u_puts(" KB");
	}
}

static void print_entry(const struct sys_dirent *d)
{
	unsigned int i, n;
	char buf[BUF_SIZE];

	if (!d->raw_83) {
		u_puts(d->name);
		if (d->is_dir) {
			u_puts("  <DIR>\n");
			return;
		}
		u_putc(' ');
	} else {
		for (i = 0; i < 11 && d->name[i]; i++)
			u_putc(d->name[i] == ' ' ? ' ' : d->name[i]);
		if (i < 11)
			u_putc(' ');
		if (d->is_dir) {
			u_puts("  <DIR>\n");
			return;
		}
		u_puts(" ");
	}
	n = d->size;
	if (n == 0) {
		u_putc('0');
	} else {
		i = BUF_SIZE - 1;
		buf[i] = '\0';
		while (n && i > 0) {
			buf[--i] = (char)('0' + (n % 10));
			n /= 10;
		}
		u_puts(buf + i);
	}
	u_putc('\n');
}

void _start(const char *path)
{
	char cwd[VFS_PATH_MAX];

	/* The shell passes arguments verbatim; an empty one means the current
	 * working directory (and "/" is the mount listing). */
	if (!path || !path[0]) {
		if (u_getcwd(cwd, sizeof(cwd)) != 0)
			cwd[0] = '\0';
		path = cwd;
	}

	if (!path || !path[0] || (path[0] == '/' && path[1] == '\0')) {
		struct sys_mount m;
		unsigned int i = 0;

		while (u_syscall2(SYS_LIST_MOUNTS, (int)i, (int)&m) == 1) {
			u_puts(m.name);
			u_puts("  ");
			u_puts(m.type);
			u_putc(' ');
			put_size_sectors(m.total_sectors);
			if (m.label[0]) {
				u_puts("  \"");
				u_puts(m.label);
				u_putc('"');
			}
			u_putc('\n');
			i++;
		}
		u_exit(0);
	}

	{
		struct sys_dirent d;
		unsigned int i = 0;
		int r;

		while ((r = u_syscall3(SYS_LIST_DIR, (int)path, (int)i,
				       (int)&d)) == 1) {
			print_entry(&d);
			i++;
		}
		if (r == SYS_ENOENT)
			u_puts("ls: no such file or directory\n");
		else if (r == SYS_ENODEV)
			u_puts("ls: no such device\n");
		else if (r == SYS_ENOFS)
			u_puts("ls: no filesystem on device\n");
		else if (r == SYS_EINVAL)
			u_puts("ls: not a directory\n");
		else if (r == SYS_EIO)
			u_puts("ls: disk error\n");
	}
	u_exit(0);
}
