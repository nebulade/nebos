/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch.
   Commands are spawned as separate processes for preemptive multitasking. */

#include "io.h"
#include "power.h"
#include "bootcon.h"
#include "dbg.h"
#include "idle.h"
#include "kexec.h"
#include "kmalloc.h"
#include "mm.h"
#include "process.h"
#include "sys_fs.h"
#include "terminal.h"
#include "timer.h"

extern void program_reboot_main(void);
extern void program_poweroff_main(void);
extern void program_display_main(void);
extern void program_gfxbench_main(void);
extern void program_serial_loop_main(const char *arg);
extern void program_lsusb_main(void);
extern void program_netdev_main(const char *arg);
extern void program_format_main(const char *arg);
extern void program_lspci_main(const char *filter);
extern void program_peek_main(const char *arg);
extern void program_poke_main(const char *arg);



/* ---- Command registry ----
 *
 * One table drives `help` and the privileged kernel commands that the ring-3
 * terminal spawns through SYS_SPAWN_KCMD.  The ring-3 shell owns its own
 * builtins, Tab completion and PATH lookup. */

enum cmd_kind {
	CK_SPAWN = 0,        /* spawn, no arguments */
	CK_SPAWN_ARG,        /* spawn with the raw remainder of the line */
	CK_CUSTOM            /* handled by a cmd_*() handler below */
};

#define CMD_REQARG 0x01  /* the argument must be non-empty */
#define CMD_BUILTIN 0x04 /* handled inline, before the PATH lookup */

struct kcmd {
	const char *name;               /* dispatch token */
	const char *usage;              /* `help` line (and completion name) */
	unsigned char kind;
	unsigned char flags;
	void (*fn)(void);               /* spawned entry, no argument */
	void (*fn_arg)(const char *);   /* spawned entry with an argument */
	const char *fixed;              /* CK_SPAWN: fixed argument, if any */
	int (*custom)(struct term *t, const char *args);
};

/* The terminal's spawn/path helpers are defined later in this file. */

static int cmd_bootlog(struct term *t, const char *args);
static int cmd_dbg(struct term *t, const char *args);
static int cmd_format(struct term *t, const char *args);
static int cmd_help(struct term *t, const char *args);
static int cmd_idle(struct term *t, const char *args);
static int cmd_cpufreq(struct term *t, const char *args);
static int cmd_power(struct term *t, const char *args);
static int cmd_kexec(struct term *t, const char *args);
static int cmd_kill(struct term *t, const char *args);
static int cmd_poweroff(struct term *t, const char *args);
static int cmd_ps(struct term *t, const char *args);
static int cmd_renice(struct term *t, const char *args);
static int cmd_sched(struct term *t, const char *args);
static int cmd_reboot(struct term *t, const char *args);
static int cmd_serial(struct term *t, const char *args);

/* Table order defines the `help` layout (printed as two columns).  Ring-3
 * commands are not listed here: they are files on the PATH. */
static const struct kcmd cmds[] = {
	{ "help",       "help",                          CK_CUSTOM, CMD_BUILTIN, 0, 0, 0, cmd_help },
	{ "bootlog",    "bootlog",                       CK_CUSTOM, CMD_BUILTIN, 0, 0, 0, cmd_bootlog },
	{ "reboot",     "reboot",                        CK_CUSTOM, 0, 0, 0, 0, cmd_reboot },
	{ "poweroff",   "poweroff",                      CK_CUSTOM, 0, 0, 0, 0, cmd_poweroff },
	{ "display",    "display",                       CK_SPAWN, 0, program_display_main, 0, 0, 0 },
	{ "gfxbench",   "gfxbench",                      CK_SPAWN, 0, program_gfxbench_main, 0, 0, 0 },
	{ "lsusb",      "lsusb",                         CK_SPAWN, 0, program_lsusb_main, 0, 0, 0 },
	{ "netdev",     "netdev [name]",                 CK_SPAWN_ARG, 0, 0, program_netdev_main, 0, 0 },
	{ "format",     "format <device> [size]",        CK_CUSTOM, 0, 0, 0, 0, cmd_format },
	{ "lspci",      "lspci [id]",                    CK_SPAWN_ARG, 0, 0, program_lspci_main, 0, 0 },
	{ "peek",       "peek <addr> [len]",             CK_SPAWN_ARG, 0, 0, program_peek_main, 0, 0 },
	{ "poke",       "poke <addr> <val>",             CK_SPAWN_ARG, CMD_REQARG, 0, program_poke_main, 0, 0 },
	{ "ps",         "ps",                            CK_CUSTOM, 0, 0, 0, 0, cmd_ps },
	{ "sched",      "sched",                         CK_CUSTOM, 0, 0, 0, 0, cmd_sched },
	{ "power",      "power",                         CK_CUSTOM, 0, 0, 0, 0, cmd_power },
	{ "cpufreq",    "cpufreq [auto|min|max|off|n]",  CK_CUSTOM, 0, 0, 0, 0, cmd_cpufreq },
	{ "idle",       "idle [auto|hlt|c1e|c3|c6]",     CK_CUSTOM, 0, 0, 0, 0, cmd_idle },
	{ "renice",     "renice <pid> <weight>",         CK_CUSTOM, CMD_REQARG, 0, 0, 0, cmd_renice },
	{ "kill",       "kill <pid>",                    CK_CUSTOM, 0, 0, 0, 0, cmd_kill },
	{ "kexec",      "kexec",                         CK_CUSTOM, 0, 0, 0, 0, cmd_kexec },
	{ "dbg",        "dbg",                           CK_CUSTOM, 0, 0, 0, 0, cmd_dbg },
	{ "serial",     "serial test",                   CK_CUSTOM, 0, 0, 0, 0, cmd_serial },
};

#define NCMDS (sizeof(cmds) / sizeof(cmds[0]))

/* Find the registry entry whose name matches the first word of the line. */


/* Split the line into the command word and the trimmed remainder. */

/* Dispatch one registry entry.  Returns 1 when the handler already printed the
 * next prompt (clear), 0 otherwise. */

/* `help`: printed as two 32-column columns so it fits the 64-column terminal
 * without scrolling. */
static void help_item_pad(const char *s)
{
	unsigned int n = 0;

	while (s[n]) {
		io_putchar(s[n]);
		n++;
	}
	while (n < 32) {
		io_putchar(' ');
		n++;
	}
}

static void help_print(void)
{
	unsigned int nitems = NCMDS;
	unsigned int half = (nitems + 1) / 2;
	unsigned int k;

	for (k = 0; k < half; k++) {
		if (k + half < nitems) {
			help_item_pad(cmds[k].usage);
			io_putstr(cmds[k + half].usage);
		} else {
			io_putstr(cmds[k].usage);
		}
		io_putchar('\n');
	}
}

/* ---- Command handlers that run inline in the terminal ---- */



/* Dump the whole boot console text captured by bootcon_puts().  The ring-3
 * terminal keeps scrollback, so no paging is needed; sleep briefly every
 * 512 bytes so the terminal can drain its tty ring while we write. */
static int cmd_bootlog(struct term *t, const char *args)
{
	const char *p;
	unsigned int len, i;

	(void)t;
	p = bootcon_log(&len);
	while (*args == ' ')
		args++;
	if (args[0] == 's' && args[1] == 'a' && args[2] == 'v' &&
	    args[3] == 'e' && (args[4] == ' ' || args[4] == '\0')) {
		const char *path = args + 4;
		int r;

		while (*path == ' ')
			path++;
		if (!*path) {
			io_putstr("bootlog: usage bootlog save <path>\n");
			return 0;
		}
		r = sys_write_file(path, p, len);
		if (r == 0)
			io_putstr("bootlog saved\n");
		else {
			io_putstr("bootlog: save failed (");
			io_puthex((unsigned int)r);
			io_putstr(")\n");
		}
		return 0;
	}

	for (i = 0; i < len; i++) {
		io_putchar(p[i]);
		if ((i & 511u) == 511u)
			process_sleep_ms(1);
	}
	if (bootcon_log_truncated())
		io_putstr("\n[boot log truncated]\n");
	return 0;
}

static int cmd_dbg(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	program_dbg_main();
	return 0;
}

static int cmd_format(struct term *t, const char *args)
{
	(void)t;
	program_format_main(args);
	return 0;
}

/* `path` builtin: show or set the command search path. */


/* List the command files found on the search path. */
static void help_print_path(struct term *t)
{
	const char *p = t->path;

	while (*p) {
		char dir[VFS_PATH_MAX];
		unsigned int n = 0, col = 0, i;

		while (*p && *p != ':' && n + 1 < sizeof(dir))
			dir[n++] = *p++;
		dir[n] = '\0';
		if (*p == ':')
			p++;
		if (dir[0] == '\0')
			continue;

		io_putstr(dir);
		io_putstr(":\n");
		for (i = 0; ; i++) {
			struct vfs_dirent d;
			unsigned int len = 0;
			int r = vfs_list_dir(dir, i, &d);

			if (r != 1)
				break;
			if (d.is_dir)
				continue;
			while (d.name[len])
				len++;
			if (col > 0 && col + 2 + len > t->screen.cols) {
				io_putchar('\n');
				col = 0;
			}
			if (col > 0) {
				io_putstr("  ");
				col += 2;
			}
			io_putstr(d.name);
			col += len;
		}
		io_putchar('\n');
	}
}

static int cmd_help(struct term *t, const char *args)
{
	(void)args;
	io_putstr("shell builtins: cd path clear\n");
	help_print();
	help_print_path(t);
	return 0;
}

static int cmd_kexec(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	if (kexec_busy) {
		io_putstr("kexec: already receiving\n");
		return 0;
	}
	kexec_busy = 1;
	if (process_create("kexec", program_kexec_main) < 0) {
		kexec_busy = 0;
		io_putstr("error: too many processes\n");
	}
	return 0;
}

static int cmd_kill(struct term *t, const char *args)
{
	unsigned int pid = 0;
	int has_digit = 0;

	(void)t;
	while (*args >= '0' && *args <= '9') {
		pid = pid * 10 + (unsigned int)(*args - '0');
		args++;
		has_digit = 1;
	}
	while (*args == ' ')
		args++;
	if (!has_digit || *args) {
		io_putstr("kill: usage kill <pid>\n");
		return 0;
	}
	if (process_kill(pid) < 0)
		io_putstr("kill: no such process\n");
	else
		io_putstr("killed\n");
	return 0;
}

static int cmd_poweroff(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	program_poweroff_main();
	return 0;
}

static int cmd_ps(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	process_ps();
	return 0;
}

static int cmd_sched(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	sched_print();
	return 0;
}

static int arg_is(const char *args, const char *word)
{
	unsigned int i = 0;

	while (word[i] && args[i] == word[i])
		i++;
	return word[i] == '\0' && args[i] == '\0';
}

static int cmd_idle(struct term *t, const char *args)
{
	(void)t;
	if (!args[0] || arg_is(args, "status")) {
		io_putstr("idle ");
		io_putstr(idle_mode_name(idle_mode()));
		if (!idle_mwait_supported())
			io_putstr(" (mwait unsupported)");
		io_putchar('\n');
		return 0;
	}
	if (arg_is(args, "auto")) {
		idle_set_mode(IDLE_AUTO);
		io_putstr("idle auto\n");
		return 0;
	}
	if (arg_is(args, "hlt")) {
		idle_set_mode(IDLE_C1);
		io_putstr("idle hlt\n");
		return 0;
	}
	{
		int mode = -1;

		if (arg_is(args, "c1e"))
			mode = IDLE_C1E;
		else if (arg_is(args, "c3"))
			mode = IDLE_C3;
		else if (arg_is(args, "c6"))
			mode = IDLE_C6;
		else if (arg_is(args, "c3ap"))
			mode = IDLE_C3_AP;
		else if (arg_is(args, "c6ap"))
			mode = IDLE_C6_AP;
		if (mode >= 0) {
			if (idle_set_mode(mode) < 0) {
				io_putstr("idle: mwait not available\n");
				return 0;
			}
			io_putstr("idle ");
			io_putstr(idle_mode_name(mode));
			io_putstr(" (test the keyboard; power-cycle if it hangs)\n");
			return 0;
		}
	}
	io_putstr("idle: usage idle [auto|hlt|c1e|c3|c6|c3ap|c6ap]\n");
	return 0;
}

static int cmd_power(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	power_print();
	return 0;
}

static int cmd_cpufreq(struct term *t, const char *args)
{
	(void)t;
	power_cpufreq_command(args);
	return 0;
}

static int cmd_renice(struct term *t, const char *args)
{
	unsigned int pid = 0, weight = 0;
	int have_pid = 0, have_weight = 0;

	(void)t;
	while (*args >= '0' && *args <= '9') {
		pid = pid * 10 + (unsigned int)(*args - '0');
		args++;
		have_pid = 1;
	}
	while (*args == ' ')
		args++;
	while (*args >= '0' && *args <= '9') {
		weight = weight * 10 + (unsigned int)(*args - '0');
		args++;
		have_weight = 1;
	}
	while (*args == ' ')
		args++;
	if (!have_pid || !have_weight || *args ||
	    weight < 1 || weight > SCHED_WEIGHT_MAX) {
		io_putstr("renice: usage renice <pid> <1..16>\n");
		return 0;
	}
	if (process_set_weight(pid, weight) < 0)
		io_putstr("renice: no such process\n");
	else
		io_putstr("weight changed\n");
	return 0;
}

static int cmd_reboot(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	program_reboot_main();
	return 0;
}

static int cmd_serial(struct term *t, const char *args)
{
	(void)t;
	if (args[0] == 't' && args[1] == 'e' && args[2] == 's' && args[3] == 't' &&
	    args[4] == '\0') {
		if (process_create_arg("serial_loop", program_serial_loop_main, "test") < 0)
			io_putstr("error: too many processes\n");
	} else {
		io_putstr("serial: usage serial test\n");
	}
	return 0;
}

struct kcmd_ctx {
	char name[16];
	char args[VFS_PATH_MAX];
};

static const struct kcmd *kcmd_find(const char *name)
{
	unsigned int i;

	for (i = 0; i < NCMDS; i++)
		if (cmds[i].name[0] == name[0]) {
			unsigned int j = 0;
			while (cmds[i].name[j] && name[j] && cmds[i].name[j] == name[j])
				j++;
			if (!cmds[i].name[j] && !name[j])
				return &cmds[i];
		}
	return 0;
}

static void kcmd_entry(void *arg)
{
	struct kcmd_ctx *ctx = (struct kcmd_ctx *)arg;
	const struct kcmd *c = kcmd_find(ctx->name);

	if (c) {
		if (c->kind == CK_CUSTOM) {
			struct term *t = (struct term *)mm_alloc_pages(2);
			unsigned int i;

			if (!t) {
				io_putstr("kcmd: out of memory\n");
			} else {
				unsigned char *p = (unsigned char *)t;

				for (i = 0; i < 2 * PAGE_SIZE; i++)
					p[i] = 0;
				/* Kernel-command output has no cell screen; help uses
				 * this width to lay out its columns on the tty. */
				t->screen.cols = 64;
				t->screen.rows = 32;
				for (i = 0; i + 1 < sizeof(t->cwd) &&
					    current_proc()->cwd[i]; i++)
					t->cwd[i] = current_proc()->cwd[i];
				t->cwd[i] = '\0';
				{
					const char *def = "/ramfs/bin";
					for (i = 0; def[i] && i + 1 < sizeof(t->path); i++)
						t->path[i] = def[i];
					t->path[i] = '\0';
				}
				c->custom(t, ctx->args);
				mm_free_page((unsigned int)t);
				mm_free_page((unsigned int)t + PAGE_SIZE);
			}
		} else if (c->kind == CK_SPAWN) {
			if (c->fixed)
				c->fn_arg(c->fixed);
			else
				c->fn();
		} else if (c->kind == CK_SPAWN_ARG) {
			c->fn_arg(ctx->args);
		}
	} else {
		io_putstr(ctx->name);
		io_putstr(": no such kernel command\n");
	}
	kfree(ctx);
	process_exit();
}

int term_kcmd_spawn(const char *name, const char *args, unsigned int tty)
{
	const struct kcmd *c = kcmd_find(name);
	struct kcmd_ctx *ctx;
	unsigned int i;
	int pid;

	if (!c || ((c->flags & CMD_BUILTIN) && c->custom != cmd_help &&
		   c->custom != cmd_bootlog))
		return -1;
	ctx = (struct kcmd_ctx *)kmalloc(sizeof(*ctx));
	if (!ctx)
		return -1;
	for (i = 0; i + 1 < sizeof(ctx->name) && name[i]; i++)
		ctx->name[i] = name[i];
	ctx->name[i] = '\0';
	for (i = 0; i + 1 < sizeof(ctx->args) && args[i]; i++)
		ctx->args[i] = args[i];
	ctx->args[i] = '\0';
	pid = process_create_ptr("kcmd", kcmd_entry, ctx);
	if (pid < 0) {
		kfree(ctx);
		return -1;
	}
	if (tty)
		process_set_tty((unsigned int)pid, (int)tty);
	return pid;
}

