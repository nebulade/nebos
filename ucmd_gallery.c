/* gallery launcher: the real app is a disk image (GALLERY) because the
 * kernel's embedded command pack is close to its low-memory wall.  This tiny
 * command keeps `gallery` on PATH; the app itself links ngui and the font. */

#include "ucmd_lib.h"

static const char *const candidates[] = {
	"/share/GALLERY",
	"/share/GALLERY.ELF",
	"/floppy1/GALLERY",
	"/floppy1/GALLERY.ELF",
	"/ide0p1/GALLERY",
	"/sata0p1/GALLERY",
	"/ide0/GALLERY",
	"/sata0/GALLERY",
	0
};

void _start(const char *arg)
{
	char cwd[64];
	char argbuf[64];
	unsigned int i, n = 0;

	while (arg && arg[n] && n + 1 < sizeof(argbuf)) {
		argbuf[n] = arg[n];
		n++;
	}
	argbuf[n] = '\0';

	if (u_getcwd(cwd, sizeof(cwd)) != 0 || !cwd[0]) {
		cwd[0] = '/';
		cwd[1] = '\0';
	}

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]) &&
		     candidates[i]; i++) {
		int pid = u_spawn(candidates[i], argbuf, 0, cwd);

		if (pid > 0) {
			u_puts("gallery: started ");
			u_puts(candidates[i]);
			u_puts("\n");
			u_exit(0);
		}
	}
	u_puts("gallery: GALLERY is not installed on a mounted disk\n");
	u_puts("  put gallery.elf on the root floppy or /share\n");
	u_exit(1);
}
