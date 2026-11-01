/* Terminal-in-a-window: per-instance state and the prompt/echo loop. */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "screen.h"
#include "vfs.h"

#define CURSOR_MS    500
#define LINE_MAX     64
#define HISTORY_SIZE 32
#define KEY_BUF_SIZE 16

struct term {
	struct text_screen screen;
	char history[HISTORY_SIZE][LINE_MAX];
	unsigned int history_count;
	unsigned int history_next;
	char line[LINE_MAX];
	unsigned int line_len;
	unsigned int line_pos;
	unsigned int line_start;
	unsigned int history_browse;
	int fg_pid;                 /* foreground child pid, or -1 when none */
	char cwd[VFS_PATH_MAX];     /* absolute current working directory */
	char path[VFS_PATH_MAX];    /* command search path (colon separated) */
	/* Per-terminal key queue: WM (slot 0) pushes, terminal process pops. */
	int keys[KEY_BUF_SIZE];
	unsigned int key_head;
	unsigned int key_tail;
};

/* Entry point for a terminal process (arg = struct term *). */
void term_entry(void *arg);
void term_run(struct term *t);

/* Ring-3 terminal: run a privileged kernel command as a kernel process with
 * output on `tty` (the command table and handlers stay in terminal.c). */
int term_kcmd_spawn(const char *name, const char *args, unsigned int tty);

/* Single-producer (WM) / single-consumer (terminal) key queue. */
static inline void term_key_push(struct term *t, int k)
{
	unsigned int next = (t->key_head + 1) % KEY_BUF_SIZE;
	if (next != t->key_tail) {
		t->keys[t->key_head] = k;
		t->key_head = next;
	}
}

static inline int term_key_pop(struct term *t)
{
	int k;
	if (t->key_tail == t->key_head)
		return 0;
	k = t->keys[t->key_tail];
	t->key_tail = (t->key_tail + 1) % KEY_BUF_SIZE;
	return k;
}

#endif
