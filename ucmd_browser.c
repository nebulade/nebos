/* browser launcher: the real browser is a large disk app (BROWSER on a FAT
 * mount) because it links ngui, the full image library, TLS and uhttp.  This
 * tiny command keeps `browser` on PATH and in the WM's Web button. */

#include "ucmd_lib.h"

/* The sentinel is part of the array; do not size it to the number of
 * candidate strings or the loop reads past the end (that was a user #PF at
 * 0x40001000 in the first net-dev build). */
static const char *const candidates[] = {
	"/share/BROWSER",
	"/share/BROWSER.ELF",
	"/floppy1/BROWSER",
	"/floppy1/BROWSER.ELF",
	"/ide0p1/BROWSER",
	"/sata0p1/BROWSER",
	"/ide0/BROWSER",
	"/sata0/BROWSER",
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
			u_puts("browser: started ");
			u_puts(candidates[i]);
			u_puts("\n");
			u_exit(0);
		}
	}
	u_puts("browser: BROWSER is not installed on a mounted disk\n");
	u_puts("  put browser.elf on the root floppy or /share\n");
	u_exit(1);
}
