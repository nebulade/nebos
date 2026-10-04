/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch.
   Commands are spawned as separate processes for preemptive multitasking. */

#include "io.h"
#include "dbg.h"
#include "idle.h"
#include "kexec.h"
#include "process.h"
#include "terminal.h"
#include "timer.h"

extern void program_hello_main(void);
extern void program_reboot_main(void);
extern void program_poweroff_main(void);
extern void program_ls_main(const char *path);
extern void program_cat_main(const char *filename);
extern void program_run_main(const char *filename);
extern void program_save_main(const char *arg);
extern void program_rm_main(const char *arg);
extern void program_mkdir_main(const char *arg);
extern void program_rmdir_main(const char *arg);
extern void program_clock_main(void);
extern void program_colors_main(void);
extern void program_display_main(void);
extern void program_cpuinfo_main(void);
extern void program_mouseinfo_main(void);
extern void program_serial_loop_main(const char *arg);
extern void program_serial_recv_main(const char *arg);
extern void program_run_serial_main(const char *arg);
extern void program_lsusb_main(void);
extern void program_storage_main(void);
extern void program_format_main(const char *arg);

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

static int is_cmd_hello(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'h' || line[i+1] != 'e' || line[i+2] != 'l' || line[i+3] != 'l' || line[i+4] != 'o')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_reboot(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'e' || line[i+2] != 'b' || line[i+3] != 'o' || line[i+4] != 'o' || line[i+5] != 't')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_poweroff(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 8 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'o' || line[i+2] != 'w' || line[i+3] != 'e' ||
	    line[i+4] != 'r' || line[i+5] != 'o' || line[i+6] != 'f' || line[i+7] != 'f')
		return 0;
	i += 8;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_ls(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's')
		return 0;
	i += 2;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "ls" must be a whole word, so reject "lsusb" etc. */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			path_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_cd(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'd')
		return 0;
	i += 2;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "cd" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			path_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_clock(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'l' || line[i+2] != 'o' ||
	    line[i+3] != 'c' || line[i+4] != 'k')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_colors(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'o' || line[i+2] != 'l' ||
	    line[i+3] != 'o' || line[i+4] != 'r' || line[i+5] != 's')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_display(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'i' || line[i+2] != 's' || line[i+3] != 'p' ||
	    line[i+4] != 'l' || line[i+5] != 'a' || line[i+6] != 'y')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_cpuinfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'p' || line[i+2] != 'u' || line[i+3] != 'i' ||
	    line[i+4] != 'n' || line[i+5] != 'f' || line[i+6] != 'o')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_mouseinfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 9 > line_len)
		return 0;
	if (line[i] != 'm' || line[i+1] != 'o' || line[i+2] != 'u' ||
	    line[i+3] != 's' || line[i+4] != 'e' || line[i+5] != 'i' ||
	    line[i+6] != 'n' || line[i+7] != 'f' || line[i+8] != 'o')
		return 0;
	i += 9;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_lsusb(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's' || line[i+2] != 'u' ||
	    line[i+3] != 's' || line[i+4] != 'b')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_space(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'p' || line[i+2] != 'a' ||
	    line[i+3] != 'c' || line[i+4] != 'e')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

#define CAT_FNAME_MAX 64
static int is_cmd_cat(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'a' || line[i+2] != 't')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_run(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'u' || line[i+2] != 'n')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

#define SAVE_ARGS_MAX 128
#define RM_FNAME_MAX 64
#define LS_PATH_MAX 64
#define FORMAT_DEV_MAX 16

static int is_cmd_save(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'a' || line[i+2] != 'v' || line[i+3] != 'e')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	while (j > 0 && arg_buf[j - 1] == ' ')
		j--;
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len && j > 0);
}

static int is_cmd_rm(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'm')
		return 0;
	i += 2;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_mkdir(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'm' || line[i+1] != 'k' || line[i+2] != 'd' || line[i+3] != 'i' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_rmdir(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'm' || line[i+2] != 'd' || line[i+3] != 'i' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_storage(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 't' || line[i+2] != 'o' ||
	    line[i+3] != 'r' || line[i+4] != 'a' || line[i+5] != 'g' || line[i+6] != 'e')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_format(const char *line, unsigned int line_len, char *dev_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'f' || line[i+1] != 'o' || line[i+2] != 'r' ||
	    line[i+3] != 'm' || line[i+4] != 'a' || line[i+5] != 't')
		return 0;
	i += 6;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "format" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		dev_buf[j++] = line[i++];
	dev_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len && j > 0);
}

static int is_cmd_help(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'h' || line[i+1] != 'e' || line[i+2] != 'l' || line[i+3] != 'p')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_clear(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'l' || line[i+2] != 'e' ||
	    line[i+3] != 'a' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_ps(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 's')
		return 0;
	i += 2;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_kill(const char *line, unsigned int line_len, unsigned int *pid_out)
{
	unsigned int i = 0, num = 0;
	int has_digit = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'k' || line[i+1] != 'i' || line[i+2] != 'l' || line[i+3] != 'l')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	while (i < line_len && line[i] >= '0' && line[i] <= '9') {
		num = num * 10 + (unsigned int)(line[i] - '0');
		has_digit = 1;
		i++;
	}
	if (!has_digit)
		return 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i != line_len)
		return 0;
	*pid_out = num;
	return 1;
}

static int is_cmd_kexec(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'k' || line[i+1] != 'e' || line[i+2] != 'x' || line[i+3] != 'e' || line[i+4] != 'c')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_dbg(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'b' || line[i+2] != 'g')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_serial(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'e' || line[i+2] != 'r' ||
	    line[i+3] != 'i' || line[i+4] != 'a' || line[i+5] != 'l')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	while (j > 0 && arg_buf[j - 1] == ' ')
		j--;
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
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

/* Spawn a foreground command process and wait for it to finish before
 * printing the next prompt (so its output doesn't overwrite the prompt). */
static void term_spawn(const char *name, void (*entry)(void))
{
	int pid = process_create(name, entry);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		process_wait((unsigned int)pid);
}

static void term_spawn_arg(const char *name, void (*entry)(const char *),
			   const char *arg)
{
	int pid = process_create_arg(name, entry, arg);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		process_wait((unsigned int)pid);
}

void term_run(struct term *t)
{
	t->line_len         = 0;
	t->line_pos         = 0;
	t->line[0]          = '\0';
	t->history_browse   = 0;

	io_clear_screen();
	term_print_prompt(t, 1);

	for (;;) {
		{
			int k = term_key_pop(t);
			if (k) {
				if (k == IO_KEY_LEFT) {
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
				} else if (k == '\n') {
					char cat_fname[CAT_FNAME_MAX];
					char save_args[SAVE_ARGS_MAX];
					char rm_fname[RM_FNAME_MAX];
					char ls_path[LS_PATH_MAX];
					char format_dev[FORMAT_DEV_MAX];
					char resolved[VFS_PATH_MAX];
					char save_resolved[SAVE_ARGS_MAX + VFS_PATH_MAX];
					unsigned int kill_pid;
					int skip_prompt = 0;
					io_putchar('\n');
					history_push(t, t->line, t->line_len);
					t->history_browse = 0;
					if (is_cmd_hello(t->line, t->line_len)) {
						term_spawn("hello", program_hello_main);
					} else if (is_cmd_reboot(t->line, t->line_len)) {
						program_reboot_main();
					} else if (is_cmd_poweroff(t->line, t->line_len)) {
						program_poweroff_main();
					} else if (is_cmd_cd(t->line, t->line_len, ls_path, LS_PATH_MAX)) {
						term_cd(t, ls_path);
					} else if (is_cmd_ls(t->line, t->line_len, ls_path, LS_PATH_MAX)) {
						if (vfs_join_path(t->cwd, ls_path, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg("ls", program_ls_main, resolved);
						} else {
							io_putstr("ls: path too long\n");
						}
					} else if (is_cmd_lsusb(t->line, t->line_len)) {
						term_spawn("lsusb", program_lsusb_main);
					} else if (is_cmd_storage(t->line, t->line_len)) {
						term_spawn("storage", program_storage_main);
					} else if (is_cmd_clock(t->line, t->line_len)) {
						io_putstr("clock: not available in windowed mode yet\n");
					} else if (is_cmd_colors(t->line, t->line_len)) {
						io_putstr("colors: not available in windowed mode yet\n");
					} else if (is_cmd_cat(t->line, t->line_len, cat_fname, CAT_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, cat_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg("cat", program_cat_main, resolved);
						} else {
							io_putstr("cat: path too long\n");
						}
					} else if (is_cmd_run(t->line, t->line_len, cat_fname, CAT_FNAME_MAX)) {
						if (cat_fname[0] == 's' && cat_fname[1] == 'e' &&
						    cat_fname[2] == 'r' && cat_fname[3] == 'i' &&
						    cat_fname[4] == 'a' && cat_fname[5] == 'l' &&
						    cat_fname[6] == '\0') {
							if (process_create_arg("run_serial", program_run_serial_main, cat_fname) < 0)
								io_putstr("error: too many processes\n");
						} else {
							io_putstr("run: not available in windowed mode yet\n");
						}
					} else if (is_cmd_save(t->line, t->line_len, save_args, SAVE_ARGS_MAX)) {
						term_resolve_first(t, save_args, save_resolved, sizeof(save_resolved));
						term_spawn_arg("save", program_save_main, save_resolved);
					} else if (is_cmd_mkdir(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg("mkdir", program_mkdir_main, resolved);
						} else {
							io_putstr("mkdir: path too long\n");
						}
					} else if (is_cmd_rmdir(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg("rmdir", program_rmdir_main, resolved);
						} else {
							io_putstr("rmdir: path too long\n");
						}
					} else if (is_cmd_rm(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg("rm", program_rm_main, resolved);
						} else {
							io_putstr("rm: path too long\n");
						}
					} else if (is_cmd_space(t->line, t->line_len)) {
						io_putstr("space: not available in windowed mode yet\n");
					} else if (is_cmd_display(t->line, t->line_len)) {
						term_spawn("display", program_display_main);
					} else if (is_cmd_cpuinfo(t->line, t->line_len)) {
						term_spawn("cpuinfo", program_cpuinfo_main);
					} else if (is_cmd_mouseinfo(t->line, t->line_len)) {
						term_spawn("mouseinfo", program_mouseinfo_main);
					} else if (is_cmd_format(t->line, t->line_len, format_dev, FORMAT_DEV_MAX)) {
						program_format_main(format_dev);
					} else if (is_cmd_ps(t->line, t->line_len)) {
						process_ps();
					} else if (is_cmd_kill(t->line, t->line_len, &kill_pid)) {
						if (process_kill(kill_pid) < 0)
							io_putstr("kill: no such process\n");
						else
							io_putstr("killed\n");
					} else if (is_cmd_kexec(t->line, t->line_len)) {
						if (kexec_busy) {
							io_putstr("kexec: already receiving\n");
						} else {
							kexec_busy = 1;
							if (process_create("kexec", program_kexec_main) < 0) {
								kexec_busy = 0;
								io_putstr("error: too many processes\n");
							}
						}
					} else if (is_cmd_dbg(t->line, t->line_len)) {
						program_dbg_main();
					} else if (is_cmd_help(t->line, t->line_len)) {
						io_putstr("cd [path]\n");
						io_putstr("ls [path]\n");
						io_putstr("cat <path>\n");
						io_putstr("save <path> <text>\n");
						io_putstr("rm <path>\n");
						io_putstr("mkdir <path>\n");
						io_putstr("rmdir <path>\n");
						io_putstr("hello\n");
						io_putstr("reboot\n");
						io_putstr("poweroff\n");
						io_putstr("run <file>\n");
						io_putstr("space\n");
						io_putstr("clock\n");
						io_putstr("colors\n");
						io_putstr("display\n");
						io_putstr("cpuinfo\n");
						io_putstr("lsusb\n");
						io_putstr("mouseinfo\n");
						io_putstr("storage\n");
						io_putstr("format <device>\n");
						io_putstr("ps\n");
						io_putstr("kill <pid>\n");
						io_putstr("kexec\n");
						io_putstr("dbg\n");
						io_putstr("serial test|recv\n");
						io_putstr("clear\n");
				} else if (is_cmd_clear(t->line, t->line_len)) {
					io_clear_screen();
					term_print_prompt(t, 1);
					skip_prompt = 1;
				} else {
					char serial_arg[8];
					if (is_cmd_serial(t->line, t->line_len, serial_arg, sizeof(serial_arg))) {
						if (serial_arg[0] == 't' && serial_arg[1] == 'e' &&
						    serial_arg[2] == 's' && serial_arg[3] == 't' &&
						    serial_arg[4] == '\0') {
							if (process_create_arg("serial_loop", program_serial_loop_main, serial_arg) < 0)
								io_putstr("error: too many processes\n");
						} else if (serial_arg[0] == 'r' && serial_arg[1] == 'e' &&
						           serial_arg[2] == 'c' && serial_arg[3] == 'v' &&
						           serial_arg[4] == '\0') {
							program_serial_recv_main(serial_arg);
						} else {
							io_putstr("serial: usage serial test|recv\n");
						}
					} else {
						unsigned int i = 0;
						while (i < t->line_len && t->line[i] == ' ')
							i++;
						if (i < t->line_len)
							io_putstr("command not found\n");
					}
				}
				t->line_len = 0;
				t->line_pos = 0;
				t->line[0]  = '\0';
				if (!skip_prompt) {
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
			cpu_idle();
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
