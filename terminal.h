/* Terminal-in-a-window: per-instance state and the prompt/echo loop. */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "screen.h"
#include "vfs.h"

struct term {
	struct text_screen screen;
	char cwd[VFS_PATH_MAX];     /* absolute current working directory */
	char path[VFS_PATH_MAX];    /* command search path (colon separated) */
};

/* Ring-3 terminal: run a privileged kernel command as a kernel process with
 * output on `tty` (the command table and handlers stay in terminal.c). */
int term_kcmd_spawn(const char *name, const char *args, unsigned int tty);

#endif
