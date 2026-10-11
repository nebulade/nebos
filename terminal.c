/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch.
   Commands are spawned as separate processes for preemptive multitasking. */

#include "io.h"
#include "dbg.h"
#include "kdbg.h"
#include "kexec.h"
#include "process.h"
#include "terminal.h"
#include "timer.h"

extern unsigned char _binary_hello_elf_start[];
extern unsigned char _binary_hello_elf_end[];
extern void program_reboot_main(void);
extern void program_poweroff_main(void);
extern void program_run_main(const char *filename);
extern void program_clock_main(void);
extern void program_colors_main(void);
extern void program_display_main(void);
extern void program_mouseinfo_main(void);
extern void program_serial_loop_main(const char *arg);
extern void program_serial_recv_main(const char *arg);
extern void program_run_serial_main(const char *arg);
extern void program_run_embedded_main(const char *name);
extern void program_lsusb_main(void);
extern void program_storage_main(void);
extern void program_mem_main(void);
extern void program_format_main(const char *arg);
extern void program_lspci_main(const char *filter);
extern void program_peek_main(const char *arg);
extern void program_poke_main(const char *arg);
extern void program_netinfo_main(void);
extern void program_netrecv_main(void);
extern void program_netsend_main(void);
extern void program_netip_main(const char *arg);
extern void program_netlog_main(const char *arg);
extern void program_netconsole_main(const char *arg);
extern void program_dhcp_main(void);
extern void program_netregs_main(void);
extern void program_netloop_main(void);
extern void program_netreset_main(void);
extern void program_pong_main(void);
extern void program_ping_main(const char *arg);
extern void program_curl_main(const char *arg);
extern void program_netdns_main(const char *arg);
extern void program_host_main(const char *arg);

/* Print the prompt (with the current directory) and set line_start. */
static void term_print_prompt(struct term *t, int first)
{
	if (!first)
		io_putchar('\n');
	io_putchar('[');
	io_putstr(t->cwd);
	io_putstr("] > ");
	t->line_start = io_get_cursor();
}

/* ---- Command registry ----
 *
 * One table drives command dispatch, `help` and Tab completion.  Most commands
 * are spawned as separate kernel processes; the CK_CUSTOM handlers are the
 * shell builtins, the commands that must run inline (ps/kill/kexec/...) and the
 * ones deliberately unavailable in a windowed terminal. */

enum cmd_kind {
	CK_SPAWN = 0,        /* spawn, no arguments */
	CK_SPAWN_ARG,        /* spawn with the raw remainder of the line */
	CK_SPAWN_PATH,       /* whole remainder, resolved against the cwd */
	CK_SPAWN_FIRST,      /* first token, resolved against the cwd */
	CK_SPAWN_SAVE,       /* first token resolved, the rest kept as text */
	CK_CUSTOM            /* handled by a cmd_*() handler below */
};

#define CMD_REQARG 0x01  /* the argument must be non-empty */
#define CMD_OPTARG 0x02  /* an empty argument is allowed (CK_SPAWN_PATH) */
#define CMD_USER   0x04  /* spawn an embedded ring-3 ELF (ucmds[]) */

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
static void term_cd(struct term *t, const char *arg);
static void term_spawn(struct term *t, const char *name, void (*entry)(void));
static void term_spawn_arg(struct term *t, const char *name,
			   void (*entry)(const char *), const char *arg);
static void term_resolve_first(struct term *t, const char *args,
			       char *out, unsigned int out_size);
static void cmd_spawn_user(struct term *t, const struct kcmd *c, const char *arg);

static int cmd_cd(struct term *t, const char *args);
static int cmd_clear(struct term *t, const char *args);
static int cmd_clock(struct term *t, const char *args);
static int cmd_colors(struct term *t, const char *args);
static int cmd_dbg(struct term *t, const char *args);
static int cmd_format(struct term *t, const char *args);
static int cmd_hello(struct term *t, const char *args);
static int cmd_help(struct term *t, const char *args);
static int cmd_kexec(struct term *t, const char *args);
static int cmd_kill(struct term *t, const char *args);
static int cmd_poweroff(struct term *t, const char *args);
static int cmd_ps(struct term *t, const char *args);
static int cmd_reboot(struct term *t, const char *args);
static int cmd_run(struct term *t, const char *args);
static int cmd_serial(struct term *t, const char *args);

/* Table order defines the `help` layout (printed as two columns). */
static const struct kcmd cmds[] = {
	{ "cd",         "cd [path]",                     CK_CUSTOM, 0, 0, 0, 0, cmd_cd },
	{ "ls",         "ls [path]",                     CK_SPAWN_PATH, CMD_OPTARG | CMD_USER, 0, 0, 0, 0 },
	{ "cat",        "cat <path>",                    CK_SPAWN_FIRST, CMD_USER, 0, 0, 0, 0 },
	{ "save",       "save <path> <text>",            CK_SPAWN_SAVE, CMD_USER, 0, 0, 0, 0 },
	{ "rm",         "rm <path>",                     CK_SPAWN_FIRST, CMD_USER, 0, 0, 0, 0 },
	{ "mkdir",      "mkdir <path>",                  CK_SPAWN_FIRST, CMD_USER, 0, 0, 0, 0 },
	{ "rmdir",      "rmdir <path>",                  CK_SPAWN_FIRST, CMD_USER, 0, 0, 0, 0 },
	{ "hello",      "hello",                         CK_CUSTOM, 0, 0, 0, 0, cmd_hello },
	{ "reboot",     "reboot",                        CK_CUSTOM, 0, 0, 0, 0, cmd_reboot },
	{ "poweroff",   "poweroff",                      CK_CUSTOM, 0, 0, 0, 0, cmd_poweroff },
	{ "run",        "run <name>|<file>",             CK_CUSTOM, 0, 0, 0, 0, cmd_run },
	{ "space",      "space",                         CK_SPAWN, 0, 0, program_run_embedded_main, "space", 0 },
	{ "prog",       "prog",                          CK_SPAWN, 0, 0, program_run_embedded_main, "prog", 0 },
	{ "clock",      "clock",                         CK_CUSTOM, 0, 0, 0, 0, cmd_clock },
	{ "colors",     "colors",                        CK_CUSTOM, 0, 0, 0, 0, cmd_colors },
	{ "display",    "display",                       CK_SPAWN, 0, program_display_main, 0, 0, 0 },
	{ "cpuinfo",    "cpuinfo",                       CK_SPAWN, CMD_USER, 0, 0, 0, 0 },
	{ "lsusb",      "lsusb",                         CK_SPAWN, 0, program_lsusb_main, 0, 0, 0 },
	{ "mouseinfo",  "mouseinfo",                     CK_SPAWN, 0, program_mouseinfo_main, 0, 0, 0 },
	{ "storage",    "storage",                       CK_SPAWN, 0, program_storage_main, 0, 0, 0 },
	{ "meminfo",    "meminfo",                       CK_SPAWN, 0, program_mem_main, 0, 0, 0 },
	{ "format",     "format <device>",               CK_CUSTOM, 0, 0, 0, 0, cmd_format },
	{ "lspci",      "lspci [id]",                    CK_SPAWN_ARG, 0, 0, program_lspci_main, 0, 0 },
	{ "peek",       "peek <addr> [len]",             CK_SPAWN_ARG, 0, 0, program_peek_main, 0, 0 },
	{ "poke",       "poke <addr> <val>",             CK_SPAWN_ARG, CMD_REQARG, 0, program_poke_main, 0, 0 },
	{ "netinfo",    "netinfo",                       CK_SPAWN, 0, program_netinfo_main, 0, 0, 0 },
	{ "netrecv",    "netrecv",                       CK_SPAWN, 0, program_netrecv_main, 0, 0, 0 },
	{ "netsend",    "netsend",                       CK_SPAWN, 0, program_netsend_main, 0, 0, 0 },
	{ "netip",      "netip [a.b.c.d]",               CK_SPAWN_ARG, 0, 0, program_netip_main, 0, 0 },
	{ "ping",       "ping <host|a.b.c.d>",           CK_SPAWN_ARG, 0, 0, program_ping_main, 0, 0 },
	{ "netlog",     "netlog <text>",                 CK_SPAWN_ARG, 0, 0, program_netlog_main, 0, 0 },
	{ "netconsole", "netconsole [on|off|host <ip>]", CK_SPAWN_ARG, 0, 0, program_netconsole_main, 0, 0 },
	{ "dhcp",       "dhcp",                          CK_SPAWN, 0, program_dhcp_main, 0, 0, 0 },
	{ "netregs",    "netregs",                       CK_SPAWN, 0, program_netregs_main, 0, 0, 0 },
	{ "netloop",    "netloop",                       CK_SPAWN, 0, program_netloop_main, 0, 0, 0 },
	{ "netreset",   "netreset",                      CK_SPAWN, 0, program_netreset_main, 0, 0, 0 },
	{ "pong",       "pong",                          CK_SPAWN, 0, program_pong_main, 0, 0, 0 },
	{ "ps",         "ps",                            CK_CUSTOM, 0, 0, 0, 0, cmd_ps },
	{ "kill",       "kill <pid>",                    CK_CUSTOM, 0, 0, 0, 0, cmd_kill },
	{ "kexec",      "kexec",                         CK_CUSTOM, 0, 0, 0, 0, cmd_kexec },
	{ "dbg",        "dbg",                           CK_CUSTOM, 0, 0, 0, 0, cmd_dbg },
	{ "serial",     "serial test|recv",              CK_CUSTOM, 0, 0, 0, 0, cmd_serial },
	{ "clear",      "clear",                         CK_CUSTOM, 0, 0, 0, 0, cmd_clear },
	{ "curl",       "curl <url>",                    CK_SPAWN_ARG, 0, 0, program_curl_main, 0, 0 },
	{ "netdns",     "netdns [a.b.c.d]",              CK_SPAWN_ARG, 0, 0, program_netdns_main, 0, 0 },
	{ "host",       "host <name>",                   CK_SPAWN_ARG, 0, 0, program_host_main, 0, 0 },
	{ "help",       "help",                          CK_CUSTOM, 0, 0, 0, 0, cmd_help },
};

#define NCMDS (sizeof(cmds) / sizeof(cmds[0]))

/* Embedded ring-3 command binaries (CK_USER rows above).  Built from
 * ucmd_*.c by the Makefile; more can be added here as they move out of the
 * kernel. */
extern unsigned char _binary_ucmd_cpuinfo_elf_start[];
extern unsigned char _binary_ucmd_cpuinfo_elf_end[];
extern unsigned char _binary_ucmd_ls_elf_start[];
extern unsigned char _binary_ucmd_ls_elf_end[];
extern unsigned char _binary_ucmd_cat_elf_start[];
extern unsigned char _binary_ucmd_cat_elf_end[];
extern unsigned char _binary_ucmd_save_elf_start[];
extern unsigned char _binary_ucmd_save_elf_end[];
extern unsigned char _binary_ucmd_rm_elf_start[];
extern unsigned char _binary_ucmd_rm_elf_end[];
extern unsigned char _binary_ucmd_mkdir_elf_start[];
extern unsigned char _binary_ucmd_mkdir_elf_end[];
extern unsigned char _binary_ucmd_rmdir_elf_start[];
extern unsigned char _binary_ucmd_rmdir_elf_end[];

struct ucmd_blob {
	const char *name;
	const unsigned char *start;
	const unsigned char *end;
};

static const struct ucmd_blob ucmds[] = {
	{ "cpuinfo", _binary_ucmd_cpuinfo_elf_start, _binary_ucmd_cpuinfo_elf_end },
	{ "ls",      _binary_ucmd_ls_elf_start,      _binary_ucmd_ls_elf_end },
	{ "cat",     _binary_ucmd_cat_elf_start,     _binary_ucmd_cat_elf_end },
	{ "save",    _binary_ucmd_save_elf_start,    _binary_ucmd_save_elf_end },
	{ "rm",      _binary_ucmd_rm_elf_start,      _binary_ucmd_rm_elf_end },
	{ "mkdir",   _binary_ucmd_mkdir_elf_start,   _binary_ucmd_mkdir_elf_end },
	{ "rmdir",   _binary_ucmd_rmdir_elf_start,   _binary_ucmd_rmdir_elf_end },
};

static const struct ucmd_blob *ucmd_lookup(const char *name)
{
	unsigned int i;

	for (i = 0; i < sizeof(ucmds) / sizeof(ucmds[0]); i++) {
		const char *n = ucmds[i].name;
		unsigned int j = 0;

		while (n[j] && n[j] == name[j])
			j++;
		if (n[j] == '\0' && name[j] == '\0')
			return &ucmds[i];
	}
	return 0;
}

/* Find the registry entry whose name matches the first word of the line. */
static const struct kcmd *cmd_lookup(const char *line, unsigned int line_len)
{
	unsigned int i = 0, j, k, m;

	while (i < line_len && line[i] == ' ')
		i++;
	for (j = 0; i + j < line_len && line[i + j] != ' '; j++)
		;
	if (j == 0)
		return 0;
	for (k = 0; k < NCMDS; k++) {
		const char *n = cmds[k].name;

		for (m = 0; m < j && n[m]; m++)
			if (line[i + m] != n[m])
				break;
		if (m == j && n[m] == '\0')
			return &cmds[k];
	}
	return 0;
}

static void cmd_usage(const struct kcmd *c)
{
	io_putstr(c->name);
	io_putstr(": usage ");
	io_putstr(c->usage);
	io_putchar('\n');
}

/* Copy the first token of `args` into `out` (0 when there is none). */
static int cmd_first_token(const char *args, char *out, unsigned int out_size)
{
	unsigned int n = 0;

	while (*args && *args != ' ') {
		if (n + 1 >= out_size)
			break;
		out[n++] = *args++;
	}
	out[n] = '\0';
	return n > 0;
}

/* Dispatch one registry entry.  Returns 1 when the handler already printed the
 * next prompt (clear), 0 otherwise. */
static int term_run_command(struct term *t, const struct kcmd *c)
{
	char args[LINE_MAX + 1];
	unsigned int i = 0, n = 0;

	/* Skip the command word, capture the trimmed argument text. */
	while (i < t->line_len && t->line[i] == ' ')
		i++;
	while (i < t->line_len && t->line[i] != ' ')
		i++;
	while (i < t->line_len && t->line[i] == ' ')
		i++;
	while (i < t->line_len && n < LINE_MAX)
		args[n++] = t->line[i++];
	while (n > 0 && args[n - 1] == ' ')
		n--;
	args[n] = '\0';

	if ((c->flags & CMD_REQARG) && args[0] == '\0') {
		cmd_usage(c);
		return 0;
	}

	switch (c->kind) {
	case CK_SPAWN:
		if (c->flags & CMD_USER)
			cmd_spawn_user(t, c, c->fixed);
		else if (c->fixed)
			term_spawn_arg(t, c->name, c->fn_arg, c->fixed);
		else
			term_spawn(t, c->name, c->fn);
		break;
	case CK_SPAWN_ARG:
		if (c->flags & CMD_USER)
			cmd_spawn_user(t, c, args);
		else
			term_spawn_arg(t, c->name, c->fn_arg, args);
		break;
	case CK_SPAWN_PATH: {
		char resolved[VFS_PATH_MAX];

		if (vfs_join_path(t->cwd, args, resolved, sizeof(resolved)) != 0) {
			io_putstr(c->name);
			io_putstr(": path too long\n");
			break;
		}
		if (c->flags & CMD_USER)
			cmd_spawn_user(t, c, resolved);
		else
			term_spawn_arg(t, c->name, c->fn_arg, resolved);
		break;
	}
	case CK_SPAWN_FIRST: {
		char tok[VFS_PATH_MAX];
		char resolved[VFS_PATH_MAX];

		if (!cmd_first_token(args, tok, sizeof(tok))) {
			cmd_usage(c);
			break;
		}
		if (vfs_join_path(t->cwd, tok, resolved, sizeof(resolved)) != 0) {
			io_putstr(c->name);
			io_putstr(": path too long\n");
			break;
		}
		if (c->flags & CMD_USER)
			cmd_spawn_user(t, c, resolved);
		else
			term_spawn_arg(t, c->name, c->fn_arg, resolved);
		break;
	}
	case CK_SPAWN_SAVE: {
		char resolved[LINE_MAX + VFS_PATH_MAX];

		if (args[0] == '\0') {
			cmd_usage(c);
			break;
		}
		term_resolve_first(t, args, resolved, sizeof(resolved));
		if (c->flags & CMD_USER)
			cmd_spawn_user(t, c, resolved);
		else
			term_spawn_arg(t, c->name, c->fn_arg, resolved);
		break;
	}
	case CK_CUSTOM:
		return c->custom(t, args);
	}
	return 0;
}

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

static int cmd_cd(struct term *t, const char *args)
{
	term_cd(t, args);
	return 0;
}

static int cmd_clear(struct term *t, const char *args)
{
	(void)args;
	io_clear_screen();
	term_print_prompt(t, 1);
	return 1;   /* prompt already printed */
}

static int cmd_clock(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	io_putstr("clock: not available in windowed mode yet\n");
	return 0;
}

static int cmd_colors(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	io_putstr("colors: not available in windowed mode yet\n");
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
	char dev[16];
	unsigned int n = 0;
	const char *p;

	(void)t;
	while (args[n] && args[n] != ' ' && n + 1 < sizeof(dev))
		dev[n] = args[n], n++;
	dev[n] = '\0';
	p = args + n;
	while (*p == ' ')
		p++;
	if (n == 0 || *p) {
		io_putstr("format: usage format <device>\n");
		return 0;
	}
	program_format_main(dev);
	return 0;
}

static int cmd_hello(struct term *t, const char *args)
{
	int pid;

	(void)args;
	pid = process_create_user("hello", _binary_hello_elf_start,
		(unsigned int)(_binary_hello_elf_end - _binary_hello_elf_start), 0);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
	return 0;
}

static int cmd_help(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	help_print();
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

static int cmd_reboot(struct term *t, const char *args)
{
	(void)t;
	(void)args;
	program_reboot_main();
	return 0;
}

static int cmd_run(struct term *t, const char *args)
{
	(void)t;
	if (args[0] == 's' && args[1] == 'e' && args[2] == 'r' &&
	    args[3] == 'i' && args[4] == 'a' && args[5] == 'l' &&
	    args[6] == '\0') {
		if (process_create_arg("run_serial", program_run_serial_main, "serial") < 0)
			io_putstr("error: too many processes\n");
	} else {
		io_putstr("run: not available in windowed mode yet\n");
	}
	return 0;
}

static int cmd_serial(struct term *t, const char *args)
{
	(void)t;
	if (args[0] == 't' && args[1] == 'e' && args[2] == 's' && args[3] == 't' &&
	    args[4] == '\0') {
		if (process_create_arg("serial_loop", program_serial_loop_main, "test") < 0)
			io_putstr("error: too many processes\n");
	} else if (args[0] == 'r' && args[1] == 'e' && args[2] == 'c' &&
		   args[3] == 'v' && args[4] == '\0') {
		program_serial_recv_main("recv");
	} else {
		io_putstr("serial: usage serial test|recv\n");
	}
	return 0;
}

/* Redraw line from index `from` to end, clear one cell after, set cursor to line_pos. */
static void redraw_line_from(const char *line, unsigned int line_len,
	unsigned int line_pos, unsigned int line_start, unsigned int from)
{
	unsigned int i;
	io_set_cursor(line_start + from);
	for (i = from; i < line_len; i++)
		io_putchar(line[i]);
	io_putchar(' ');
	io_set_cursor(line_start + line_pos);
}

/* Redraw entire line and clear through clear_through (so leftover chars are erased). */
static void redraw_line_clear(const char *line, unsigned int line_len,
	unsigned int line_pos, unsigned int line_start, unsigned int clear_through)
{
	unsigned int i;
	io_set_cursor(line_start);
	for (i = 0; i < line_len; i++)
		io_putchar(line[i]);
	for (i = line_len; i <= clear_through; i++)
		io_putchar(' ');
	io_set_cursor(line_start + line_pos);
}

/* Copy history entry (1 = newest) into t->line, redraw from start. */
static void history_load_into_line(struct term *t, unsigned int browse)
{
	unsigned int slot, len, old_len;
	const char *src;
	if (browse < 1 || browse > t->history_count)
		return;
	old_len = t->line_len;
	slot = (t->history_next + HISTORY_SIZE - browse) % HISTORY_SIZE;
	src = t->history[slot];
	for (len = 0; len < LINE_MAX - 1 && src[len]; len++)
		t->line[len] = src[len];
	t->line[len] = '\0';
	t->line_len = len;
	t->line_pos = len;
	redraw_line_clear(t->line, len, len, t->line_start, old_len > len ? old_len : len);
}

/* ---- Tab completion ---- */

static char lower_ch(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int prefix_ci(const char *s, const char *prefix, unsigned int plen)
{
	unsigned int i;
	for (i = 0; i < plen; i++)
		if (lower_ch(s[i]) != lower_ch(prefix[i]))
			return 0;
	return 1;
}

/* "HELLO   TXT" -> "hello.txt" (a completion-ready 8.3 display name). */
static void tab_display_name(const char *n, char *out)
{
	unsigned int bl = 0, el = 0, i, j = 0;

	for (i = 0; i < 8 && n[i] && n[i] != ' '; i++)
		bl = i + 1;
	for (i = 8; i < 11 && n[i] && n[i] != ' '; i++)
		el = i - 8 + 1;
	for (i = 0; i < bl; i++)
		out[j++] = lower_ch(n[i]);
	if (el) {
		out[j++] = '.';
		for (i = 0; i < el; i++)
			out[j++] = lower_ch(n[8 + i]);
	}
	out[j] = '\0';
}

/* Shared completion state (fat_list_dir takes no context pointer). */
static char tab_prefix[16];
static unsigned int tab_prefix_len;
static unsigned int tab_count;
static unsigned int tab_lcp_len;
static char tab_lcp[16];
static int tab_single_dir;
static fat_fs_t *tab_fs;
static unsigned int tab_dir_cluster;
static unsigned int tab_col;
static unsigned int tab_width;

static void tab_collect_cb(const char *name, int is_dir, unsigned int size)
{
	char disp[16];
	unsigned int i, n;

	(void)size;
	if (name[0] == '.')                 /* "." / ".." */
		return;
	tab_display_name(name, disp);
	if (!prefix_ci(disp, tab_prefix, tab_prefix_len))
		return;
	n = 0;
	while (disp[n])
		n++;
	if (tab_count == 0) {
		for (i = 0; i < n; i++)
			tab_lcp[i] = disp[i];
		tab_lcp_len = n;
		tab_single_dir = is_dir;
	} else {
		i = 0;
		while (i < tab_lcp_len && i < n && tab_lcp[i] == disp[i])
			i++;
		tab_lcp_len = i;
	}
	tab_count++;
}

static void tab_print_one(const char *name, int is_dir)
{
	unsigned int n = 0, w;

	while (name[n])
		n++;
	w = n + (is_dir ? 1 : 0);
	if (tab_col > 0 && tab_col + 2 + w > tab_width) {
		io_putchar('\n');
		tab_col = 0;
	}
	if (tab_col > 0) {
		io_putstr("  ");
		tab_col += 2;
	}
	io_putstr(name);
	if (is_dir)
		io_putchar('/');
	tab_col += w;
}

static void tab_print_cb(const char *name, int is_dir, unsigned int size)
{
	char disp[16];

	(void)size;
	if (name[0] == '.')
		return;
	tab_display_name(name, disp);
	if (!prefix_ci(disp, tab_prefix, tab_prefix_len))
		return;
	tab_print_one(disp, is_dir);
}

static int tab_scan_dir(const char *abs)
{
	fat_fs_t *fs;
	const char *fs_path;

	if (vfs_resolve(abs, &fs, &fs_path) != 0)
		return 0;
	if (fat_resolve_dir(fs, fs_path, &tab_dir_cluster) != 0)
		return 0;
	tab_fs = fs;
	tab_count = 0;
	tab_lcp_len = 0;
	fat_list_dir(fs, tab_dir_cluster, tab_collect_cb);
	return (int)tab_count;
}

static int tab_scan_mounts(void)
{
	unsigned int i;

	tab_count = 0;
	tab_lcp_len = 0;
	for (i = 0; i < vfs_mount_count; i++) {
		const char *name = vfs_mounts[i].name;
		unsigned int n = 0, j;

		while (name[n])
			n++;
		if (!prefix_ci(name, tab_prefix, tab_prefix_len))
			continue;
		if (tab_count == 0) {
			for (j = 0; j < n; j++)
				tab_lcp[j] = name[j];
			tab_lcp_len = n;
		} else {
			j = 0;
			while (j < tab_lcp_len && j < n && tab_lcp[j] == name[j])
				j++;
			tab_lcp_len = j;
		}
		tab_count++;
	}
	return (int)tab_count;
}

/* Replace line[start,end) with ins, keep the tail and redraw. */
static void term_replace(struct term *t, unsigned int start, unsigned int end,
			 const char *ins)
{
	unsigned int inslen = 0, tail, i, old_len = t->line_len;

	while (ins[inslen])
		inslen++;
	tail = old_len - end;
	if (start + inslen + tail >= LINE_MAX)
		return;
	for (i = 0; i < tail; i++)
		t->line[start + inslen + i] = t->line[end + i];
	for (i = 0; i < inslen; i++)
		t->line[start + i] = ins[i];
	t->line_len = start + inslen + tail;
	t->line[t->line_len] = '\0';
	t->line_pos = start + inslen;
	redraw_line_clear(t->line, t->line_len, t->line_pos, t->line_start,
			  old_len > t->line_len ? old_len : t->line_len);
}

/* List the ambiguous matches below the line, then redraw prompt + line. */
static void term_tab_ambiguous(struct term *t, int mounts)
{
	unsigned int i;

	io_putchar('\n');
	tab_col = 0;
	tab_width = t->screen.cols;
	if (mounts) {
		for (i = 0; i < vfs_mount_count; i++) {
			const char *name = vfs_mounts[i].name;
			if (prefix_ci(name, tab_prefix, tab_prefix_len))
				tab_print_one(name, 1);
		}
	} else {
		fat_list_dir(tab_fs, tab_dir_cluster, tab_print_cb);
	}
	io_putchar('\n');
	term_print_prompt(t, 1);
	redraw_line_clear(t->line, t->line_len, t->line_pos, t->line_start, t->line_len);
}

/* Complete the token at the cursor: command name in the first token, a VFS
 * path afterwards.  One match completes, several extend to the common
 * prefix or are listed below the line. */
static void term_complete(struct term *t)
{
	char token[LINE_MAX + 1];
	char ins[LINE_MAX + 1];
	char dir[VFS_PATH_MAX];
	unsigned int start = t->line_pos, end, tl, i, j;

	while (start > 0 && t->line[start - 1] != ' ')
		start--;
	end = t->line_pos;
	while (end < t->line_len && t->line[end] != ' ')
		end++;
	tl = end - start;
	for (i = 0; i < tl; i++)
		token[i] = t->line[start + i];
	token[tl] = '\0';

	/* First token -> command name. */
	{
		int first = 1;
		for (i = 0; i < start; i++)
			if (t->line[i] != ' ') {
				first = 0;
				break;
			}
		if (first) {
			unsigned int nitems = NCMDS;
			unsigned int count = 0, lcp_len = 0, best = 0;
			char lcp[16];

			for (i = 0; i < nitems; i++) {
				const char *n = cmds[i].name;
				unsigned int nl = 0;

				while (n[nl])
					nl++;
				if (nl < tl || !prefix_ci(n, token, tl))
					continue;
				if (count == 0) {
					for (j = 0; j < nl && j < sizeof(lcp) - 1; j++)
						lcp[j] = n[j];
					lcp_len = j;
					best = i;
				} else {
					for (j = 0; j < lcp_len && j < nl &&
						     lcp[j] == n[j]; j++)
						;
					lcp_len = j;
				}
				count++;
			}
			if (count == 0)
				return;
			if (count == 1) {
				const char *n = cmds[best].name;
				unsigned int nl = 0;

				while (n[nl])
					nl++;
				for (j = 0; j < nl && j < LINE_MAX - 1; j++)
					ins[j] = n[j];
				ins[j] = ' ';
				ins[j + 1] = '\0';
				term_replace(t, start, end, ins);
				return;
			}
			if (lcp_len > tl) {
				for (j = 0; j < lcp_len; j++)
					ins[j] = lcp[j];
				ins[lcp_len] = '\0';
				term_replace(t, start, end, ins);
				return;
			}
			io_putchar('\n');
			tab_col = 0;
			tab_width = t->screen.cols;
			for (i = 0; i < nitems; i++) {
				const char *n = cmds[i].name;
				unsigned int nl = 0;
				char name[16];

				while (n[nl])
					nl++;
				if (nl < tl || !prefix_ci(n, token, tl))
					continue;
				for (j = 0; j < nl && j < 15; j++)
					name[j] = n[j];
				name[j] = '\0';
				tab_print_one(name, 0);
			}
			io_putchar('\n');
			term_print_prompt(t, 1);
			redraw_line_clear(t->line, t->line_len, t->line_pos,
					  t->line_start, t->line_len);
			return;
		}
	}

	/* Argument token -> VFS path. */
	{
		char dirpart[LINE_MAX + 1];
		const char *last = 0, *p;
		unsigned int dp, n;
		int mounts = 0;

		for (p = token; *p; p++)
			if (*p == '/')
				last = p;
		if (last) {
			dp = (unsigned int)(last - token) + 1;
			for (i = 0; i < dp; i++)
				dirpart[i] = token[i];
			dirpart[dp] = '\0';
			p = last + 1;
		} else {
			dp = 0;
			dirpart[0] = '\0';
			p = token;
		}
		{
			unsigned int k = 0;
			while (*p && k < 15)
				tab_prefix[k++] = *p++;
			tab_prefix[k] = '\0';
			tab_prefix_len = k;
		}

		if (dirpart[0] == '/' && dirpart[1] == '\0')
			mounts = 1;
		if (dirpart[0] == '/') {
			if (dp >= VFS_PATH_MAX)
				return;
			for (i = 0; dirpart[i]; i++)
				dir[i] = dirpart[i];
			dir[i] = '\0';
		} else {
			if (vfs_join_path(t->cwd, dirpart, dir, sizeof(dir)) != 0)
				return;
		}

		n = mounts ? (unsigned int)tab_scan_mounts()
			   : (unsigned int)tab_scan_dir(dir);
		if (n == 0)
			return;
		if (dp + tab_lcp_len + 2 > LINE_MAX)
			return;
		if (n == 1) {
			for (i = 0; i < dp; i++)
				ins[i] = dirpart[i];
			for (i = 0; i < tab_lcp_len; i++)
				ins[dp + i] = tab_lcp[i];
			ins[dp + tab_lcp_len] = (mounts || tab_single_dir) ? '/' : ' ';
			ins[dp + tab_lcp_len + 1] = '\0';
			term_replace(t, start, end, ins);
			return;
		}
		if (tab_lcp_len > tab_prefix_len) {
			for (i = 0; i < dp; i++)
				ins[i] = dirpart[i];
			for (i = 0; i < tab_lcp_len; i++)
				ins[dp + i] = tab_lcp[i];
			ins[dp + tab_lcp_len] = '\0';
			term_replace(t, start, end, ins);
			return;
		}
		term_tab_ambiguous(t, mounts);
	}
}

/* Push current line to history if non-empty (and not duplicate of newest). */
static void history_push(struct term *t, const char *line, unsigned int line_len)
{
	unsigned int i, slot;
	int all_spaces = 1;
	if (line_len == 0)
		return;
	for (i = 0; i < line_len; i++) {
		if (line[i] != ' ') {
			all_spaces = 0;
			break;
		}
	}
	if (all_spaces)
		return;
	if (t->history_count > 0) {
		slot = (t->history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
		for (i = 0; i < line_len && i < LINE_MAX - 1 && t->history[slot][i]; i++) {
			if (t->history[slot][i] != line[i])
				break;
		}
		if (i == line_len && t->history[slot][i] == '\0')
			return;  /* duplicate of newest */
	}
	slot = t->history_next;
	for (i = 0; i < line_len && i < LINE_MAX - 1; i++)
		t->history[slot][i] = line[i];
	t->history[slot][i] = '\0';
	t->history_next = (t->history_next + 1) % HISTORY_SIZE;
	if (t->history_count < HISTORY_SIZE)
		t->history_count++;
}

/* Change directory: resolve `arg` (relative to t->cwd, or absolute) and
 * validate it is an existing directory. Updates t->cwd on success. */
static void term_cd(struct term *t, const char *arg)
{
	char abs[VFS_PATH_MAX];
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir;
	int r;
	unsigned int i;

	if (arg[0] == '\0') {
		t->cwd[0] = '/';
		t->cwd[1] = '\0';
		return;
	}

	if (vfs_join_path(t->cwd, arg, abs, sizeof(abs)) != 0) {
		io_putstr("cd: path too long\n");
		return;
	}

	if (abs[0] == '/' && abs[1] == '\0') {
		t->cwd[0] = '/';
		t->cwd[1] = '\0';
		return;
	}

	if (vfs_resolve(abs, &fs, &fs_path) != 0) {
		io_putstr("cd: no such directory\n");
		return;
	}
	r = fat_resolve_dir(fs, fs_path, &dir);
	if (r == -2) {
		io_putstr("cd: disk error\n");
		return;
	}
	if (r != 0) {
		io_putstr("cd: no such directory\n");
		return;
	}

	for (i = 0; abs[i] && i < sizeof(t->cwd) - 1; i++)
		t->cwd[i] = abs[i];
	t->cwd[i] = '\0';
}

/* Resolve the first token (path) of `args` against t->cwd, rebuilding
 * "abs-path rest-of-args" into out. Used by `save`. */
static void term_resolve_first(struct term *t, const char *args,
			       char *out, unsigned int out_size)
{
	char path[VFS_PATH_MAX];
	char abs[VFS_PATH_MAX];
	unsigned int i = 0, j = 0;

	while (args[i] == ' ')
		i++;
	while (args[i] && args[i] != ' ' && j < sizeof(path) - 1)
		path[j++] = args[i++];
	path[j] = '\0';

	if (vfs_join_path(t->cwd, path, abs, sizeof(abs)) != 0) {
		abs[0] = '/';
		abs[1] = '\0';
	}
	while (args[i] == ' ')
		i++;

	j = 0;
	{
		unsigned int k = 0;
		while (abs[k] && j + 1 < out_size)
			out[j++] = abs[k++];
	}
	if (args[i]) {
		if (j + 1 < out_size)
			out[j++] = ' ';
		while (args[i] && j + 1 < out_size)
			out[j++] = args[i++];
	}
	out[j] = '\0';
}

/* Spawn a foreground command process; the caller (term_run's wait loop)
 * waits for it to finish and handles Ctrl+C. */
static void term_spawn(struct term *t, const char *name, void (*entry)(void))
{
	int pid = process_create(name, entry);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
}

static void term_spawn_arg(struct term *t, const char *name,
			   void (*entry)(const char *), const char *arg)
{
	int pid = process_create_arg(name, entry, arg);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
}

/* Spawn an embedded ring-3 command (CMD_USER rows); `arg` may be NULL. */
static void cmd_spawn_user(struct term *t, const struct kcmd *c, const char *arg)
{
	const struct ucmd_blob *b = ucmd_lookup(c->name);
	int pid;

	if (!b) {
		io_putstr(c->name);
		io_putstr(": no embedded image\n");
		return;
	}
	pid = process_create_user(c->name, b->start,
		(unsigned int)(b->end - b->start), arg);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
}

void term_run(struct term *t)
{
	t->line_len         = 0;
	t->line_pos         = 0;
	t->line[0]          = '\0';
	t->history_browse   = 0;
	t->fg_pid           = -1;

	io_clear_screen();
	term_print_prompt(t, 1);

	for (;;) {
		if (t->fg_pid >= 0) {
			/* Waiting on a foreground child: poll keys for Ctrl+C
			 * and watch for the child to exit. */
			int k = term_key_pop(t);
			if (k == IO_KEY_CTRL_C) {
				int killed = (process_kill((unsigned int)t->fg_pid) == 0);
				t->fg_pid = -1;
				if (killed)
					io_putstr("^C\n");
				term_print_prompt(t, 0);
			} else if (!process_is_alive((unsigned int)t->fg_pid)) {
				t->fg_pid = -1;
				term_print_prompt(t, 0);
			} else {
				process_sleep_ms(10);
			}
			continue;
		}
		{
			int k = term_key_pop(t);
			if (k) {
				kdbg_term_count++;
				kdbg_term_last = (unsigned int)k;
				kdbg_status();
				if (k == IO_KEY_CTRL_C) {
					/* Ctrl+C at the prompt: discard the line. */
					io_putstr("^C\n");
					t->line_len = 0;
					t->line_pos = 0;
					t->line[0]  = '\0';
					term_print_prompt(t, 0);
				} else if (k == IO_KEY_LEFT) {
					if (t->line_pos > 0) {
						t->line_pos--;
						io_set_cursor(t->line_start + t->line_pos);
					}
				} else if (k == IO_KEY_RIGHT) {
					if (t->line_pos < t->line_len) {
						t->line_pos++;
						io_set_cursor(t->line_start + t->line_pos);
					}
				} else if (k == IO_KEY_UP) {
					if (t->history_count > 0) {
						if (t->history_browse < t->history_count)
							t->history_browse++;
						history_load_into_line(t, t->history_browse);
					}
				} else if (k == IO_KEY_DOWN) {
					if (t->history_browse > 0) {
						t->history_browse--;
						if (t->history_browse == 0) {
							unsigned int clear_len = t->line_len;
							t->line_len = 0;
							t->line_pos = 0;
							t->line[0]  = '\0';
							redraw_line_clear(t->line, 0, 0, t->line_start, clear_len);
						} else {
							history_load_into_line(t, t->history_browse);
						}
					}
				} else if (k == '\b') {
					if (t->line_pos > 0) {
						unsigned int i;
						t->line_pos--;
						for (i = t->line_pos; i < t->line_len; i++)
							t->line[i] = t->line[i + 1];
						t->line_len--;
						t->line[t->line_len] = '\0';
						redraw_line_from(t->line, t->line_len, t->line_pos, t->line_start, t->line_pos);
					}
				} else if (k == '\t') {
					term_complete(t);
				} else if (k == '\n') {
					int skip_prompt = 0;
					io_putchar('\n');
					history_push(t, t->line, t->line_len);
					t->history_browse = 0;
					{
						const struct kcmd *c =
							cmd_lookup(t->line, t->line_len);

						if (c) {
							skip_prompt = term_run_command(t, c);
						} else {
							unsigned int si = 0;

							while (si < t->line_len && t->line[si] == ' ')
								si++;
							if (si < t->line_len)
								io_putstr("command not found\n");
						}
					}
					t->line_len = 0;
					t->line_pos = 0;
					t->line[0]  = '\0';
					if (!skip_prompt && t->fg_pid < 0) {
						term_print_prompt(t, 0);
					}
				} else {
				unsigned char u = (unsigned char)k;
				if (u >= 0x20 && u < 0x7F && t->line_len < LINE_MAX - 1) {
					unsigned int i;
					t->history_browse = 0;
					for (i = t->line_len; i > t->line_pos; i--)
						t->line[i] = t->line[i - 1];
					t->line[t->line_pos] = (char)k;
					t->line_len++;
					t->line[t->line_len] = '\0';
					t->line_pos++;
					redraw_line_from(t->line, t->line_len, t->line_pos, t->line_start, t->line_pos - 1);
				}
			}
		} else {
			process_sleep_ms(10);
		}
	}
}
}

void term_entry(void *arg)
{
	struct term *t = (struct term *)arg;
	process_set_screen(&t->screen);
	term_run(t);
}
