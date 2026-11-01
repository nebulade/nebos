/* Ring-3 terminal: owns a canvas window, a cell grid and the shell.  Text
 * commands write bytes to the tty; this process drains and renders them.
 * Privileged kernel commands are spawned as kernel processes (term_kcmd). */

#include "ucmd_lib.h"
#include "uwm_lib.h"
#include "io.h"
#include "vfs.h"

#define MAX_COLS 128
#define MAX_ROWS 64
#define BUF_MAX 256
#define LINE_MAX 128
#define HIST_MAX 16

static struct win_info wi;
static struct usurf surf;
static int tty;
static unsigned short cells[MAX_COLS * MAX_ROWS];
static unsigned int cols = 64, rows = 32;
static unsigned int cx, cy;
static char line[LINE_MAX];
static unsigned int line_len;
static unsigned int line_row, line_col;
static int hist_browse;
static char cwd[VFS_PATH_MAX];
static char path[VFS_PATH_MAX];
static char hist[HIST_MAX][LINE_MAX];
static unsigned int hist_count, hist_next;
static int fg_pid = -1;
static int dirty = 1;
static int cursor_on = 1;
static unsigned int last_blink;

static unsigned short *cell_at(unsigned int x, unsigned int y)
{
	return &cells[y * cols + x];
}

static void cell_put(unsigned int x, unsigned int y, char ch)
{
	*cell_at(x, y) = (unsigned short)(0x0A00 | (unsigned char)ch);
}

static void scroll_up(void)
{
	unsigned int i;

	for (i = 0; i < (rows - 1) * cols; i++)
		cells[i] = cells[i + cols];
	for (; i < rows * cols; i++)
		cells[i] = 0x0A20;
}

static void term_newline(void)
{
	cx = 0;
	cy++;
	if (cy >= rows) {
		scroll_up();
		cy = rows - 1;
	}
	dirty = 1;
}

static void term_putc(char c)
{
	if (c == '\n') {
		term_newline();
		return;
	}
	if (c == '\r') {
		cx = 0;
		dirty = 1;
		return;
	}
	if (c == '\b') {
		if (cx > 0)
			cx--;
		cell_put(cx, cy, ' ');
		dirty = 1;
		return;
	}
	if (c == '\t') {
		unsigned int n = 8 - (cx % 8);
		while (n--)
			term_putc(' ');
		return;
	}
	if (c < 0x20 || c > 0x7E)
		return;
	cell_put(cx, cy, c);
	cx++;
	if (cx >= cols)
		term_newline();
	dirty = 1;
}

static void term_puts(const char *s)
{
	while (*s)
		term_putc(*s++);
}

static void term_clear(void)
{
	unsigned int i;

	for (i = 0; i < cols * rows; i++)
		cells[i] = 0x0A20;
	cx = cy = 0;
	dirty = 1;
}

static void cursor_set(unsigned int row, unsigned int col)
{
	if (row >= rows) row = rows - 1;
	if (col >= cols) col = cols - 1;
	cy = row;
	cx = col;
	dirty = 1;
}

/* Parse the exact "ESC[row;colH" cursor sequence used by io_set_cursor. */
static int esc_cursor(const char *p, unsigned int n)
{
	unsigned int row = 0, col = 0, i = 0;

	if (n < 5 || p[0] != 0x1B || p[1] != '[')
		return 0;
	while (i < n && p[i] >= '0' && p[i] <= '9')
		row = row * 10 + (unsigned int)(p[i++] - '0');
	if (i >= n || p[i++] != ';')
		return 0;
	while (i < n && p[i] >= '0' && p[i] <= '9')
		col = col * 10 + (unsigned int)(p[i++] - '0');
	if (i >= n || p[i] != 'H')
		return 0;
	if (row < 1 || col < 1)
		return 0;
	cursor_set(row - 1, col - 1);
	return 1;
}

static void tty_feed(const char *buf, unsigned int n)
{
	unsigned int i = 0;

	while (i < n) {
		if (buf[i] == 0x1B) {
			unsigned int j = i + 1;
			char seq[24];
			unsigned int sn = 0;

			while (j < n && sn + 1 < sizeof(seq)) {
				seq[sn++] = buf[j];
				if ((buf[j] >= 'A' && buf[j] <= 'Z') ||
				    (buf[j] >= 'a' && buf[j] <= 'z'))
					break;
				j++;
			}
			seq[sn] = '\0';
			/* Rebuild with ESC prefix for the helper. */
			{
				char full[28];
				unsigned int m;

				full[0] = 0x1B;
				for (m = 0; m < sn && m + 1 < sizeof(full); m++)
					full[m + 1] = seq[m];
				full[sn + 1] = '\0';
				if (esc_cursor(full, sn + 1)) {
					i = j + 1;
					continue;
				}
			}
			if (sn > 1 && seq[sn - 1] == 'J' && seq[1] == '2') {
				term_clear();
			} else if (sn && seq[sn - 1] == 'H') {
				cursor_set(0, 0);
			}
			i = j < n ? j + 1 : n;
			continue;
		}
		term_putc(buf[i++]);
	}
}

static void drain_tty(void)
{
	char buf[BUF_MAX];
	int n;

	while ((n = u_tty_read(tty, buf, sizeof(buf))) > 0) {
		tty_feed(buf, (unsigned int)n);
		dirty = 1;
	}
}

static void render(void)
{
	if (!dirty)
		return;
	usurf_fill(&surf, 0, 0, wi.width, wi.height, 0x000000u);
	usurf_cells(&surf, cells, cols, rows, 0, 0, 0, 0, wi.width, wi.height);
	if (cursor_on && cx < cols && cy < rows)
		usurf_fill(&surf, (int)cx * 8, (int)cy * 16, 2, 16, 0xFFFFFFu);
	u_win_present();
	dirty = 0;
}

static void line_show(void)
{
	unsigned int i, x;

	cy = line_row;
	cx = line_col;
	for (i = 0; i < line_len; i++)
		term_putc(line[i]);
	for (x = cx; x < cols; x++)
		cell_put(x, cy, ' ');
	dirty = 1;
}

static void line_set(const char *s)
{
	unsigned int i = 0;

	while (s[i] && i + 1 < LINE_MAX) {
		line[i] = s[i];
		i++;
	}
	line[i] = '\0';
	line_len = i;
	line_show();
}

static void history_load(int n)
{
	unsigned int idx;

	if (n <= 0 || n > (int)hist_count)
		return;
	idx = (hist_next + HIST_MAX - (unsigned int)n) % HIST_MAX;
	line_set(hist[idx]);
}

static void prompt(void)
{
	term_putc('[');
	term_puts(cwd);
	term_puts("] > ");
	line_len = 0;
	line[0] = '\0';
	line_row = cy;
	line_col = cx;
	hist_browse = 0;
	dirty = 1;
}

static void print_num(unsigned int n)
{
	char b[12];
	unsigned int i = 0;

	if (!n) {
		term_putc('0');
		return;
	}
	while (n) {
		b[i++] = (char)('0' + n % 10);
		n /= 10;
	}
	while (i)
		term_putc(b[--i]);
}

static unsigned int parse_num(const char *s)
{
	unsigned int n = 0;

	while (*s == ' ')
		s++;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (unsigned int)(*s++ - '0');
	return n;
}

static void split_line(char *word, unsigned int wsz, char *args,
		       unsigned int asz)
{
	unsigned int i = 0, n = 0;

	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && n + 1 < wsz)
		word[n++] = line[i++];
	word[n] = '\0';
	while (line[i] == ' ')
		i++;
	n = 0;
	while (line[i] && n + 1 < asz)
		args[n++] = line[i++];
	while (n && args[n - 1] == ' ')
		n--;
	args[n] = '\0';
}

static int make_path(const char *dir, const char *word, char *out,
		     unsigned int max)
{
	unsigned int i = 0, j;

	if (word[0] == '/') {
		for (j = 0; word[j] && j + 1 < max; j++)
			out[j] = word[j];
		out[j] = '\0';
		return 0;
	}
	while (dir[i] && i + 1 < max)
		out[i] = dir[i], i++;
	if (i && out[i - 1] != '/' && i + 1 < max)
		out[i++] = '/';
	for (j = 0; word[j] && i + 1 < max; j++)
		out[i++] = word[j];
	out[i] = '\0';
	return 0;
}

static int try_spawn(const char *word, const char *args, char *used_path,
		     unsigned int used_size)
{
	char p[VFS_PATH_MAX];
	int pid;

	if (word[0] == '/') {
		for (unsigned int i = 0; word[i] && i + 1 < used_size; i++)
			used_path[i] = word[i];
		return u_spawn(word, args, tty, cwd);
	}
	/* Try each PATH component. */
	{
		unsigned int i = 0, start = 0;

		while (1) {
			unsigned int n = 0;
			char dir[VFS_PATH_MAX];

			while (path[i] && path[i] != ':') {
				if (n + 1 < sizeof(dir))
					dir[n++] = path[i];
				i++;
			}
			dir[n] = '\0';
			if (dir[0]) {
				make_path(dir, word, p, sizeof(p));
				pid = u_spawn(p, args, tty, cwd);
				if (pid >= 0) {
					for (n = 0; p[n] && n + 1 < used_size; n++)
						used_path[n] = p[n];
					used_path[n] = '\0';
					return pid;
				}
				if (pid == -2)
					return -2;
			}
			if (!path[i])
				break;
			i++;
			start = i;
			(void)start;
		}
	}
	return -1;
}

static void run_line(void)
{
	char word[LINE_MAX], args[LINE_MAX];
	char used[VFS_PATH_MAX];
	int pid;

	split_line(word, sizeof(word), args, sizeof(args));
	if (!word[0]) {
		prompt();
		return;
	}
	if (word[0] == 'c' && word[1] == 'd' && !word[2]) {
		if (u_chdir(args[0] ? args : "/") != 0)
			term_puts("cd: no such directory\n");
		u_getcwd(cwd, sizeof(cwd));
		prompt();
		return;
	}
	if (word[0] == 'p' && word[1] == 'a' && word[2] == 't' && !word[3]) {
		unsigned int i;
		if (!args[0]) {
			term_puts(path);
			term_putc('\n');
		} else {
			for (i = 0; args[i] && i + 1 < sizeof(path); i++)
				path[i] = args[i];
			path[i] = '\0';
		}
		prompt();
		return;
	}
	if (word[0] == 'h' && word[1] == 'e' && word[2] == 'l' &&
	    word[3] == 'p' && !word[4]) {
		pid = u_spawn_kcmd("help", "", tty);
		if (pid > 0) {
			fg_pid = pid;
			return;
		}
		term_puts("builtins: cd path help clear ps kill\n");
		term_puts("commands: files in ");
		term_puts(path);
		term_putc('\n');
		prompt();
		return;
	}
	if (word[0] == 'c' && word[1] == 'l' && word[2] == 'e' &&
	    word[3] == 'a' && word[4] == 'r' && !word[5]) {
		term_clear();
		prompt();
		return;
	}
	if (word[0] == 'p' && word[1] == 's' && !word[2]) {
		struct sys_procinfo pi;
		unsigned int i = 0;

		term_puts("PID  STATE  W CPU NAME\n");
		while (u_syscall2(SYS_PROCINFO, (int)i, (int)&pi) == 1) {
			print_num(pi.pid);
			term_puts("  ");
			if (pi.state == 1) term_puts("ready");
			else if (pi.state == 2) term_puts("run  ");
			else if (pi.state == 4) term_puts("sleep");
			else if (pi.state == 5) term_puts("block");
			else term_puts("?    ");
			term_puts("  ");
			print_num(pi.weight);
			term_puts("   ");
			if (pi.cpu != 0xFFFFFFFFu)
				print_num(pi.cpu);
			else
				term_putc('-');
			term_puts("   ");
			term_puts(pi.name);
			term_putc('\n');
			i++;
		}
		prompt();
		return;
	}
	if (word[0] == 'k' && word[1] == 'i' && word[2] == 'l' &&
	    word[3] == 'l' && !word[4]) {
		unsigned int pid = parse_num(args);
		if (!pid || u_kill(pid) != 0)
			term_puts("kill: no such process\n");
		prompt();
		return;
	}

	pid = try_spawn(word, args, used, sizeof(used));
	if (pid == -2)
		term_puts("not an executable\n");
	else if (pid < 0) {
		pid = u_spawn_kcmd(word, args, tty);
		if (pid < 0)
			term_puts("command not found\n");
	}
	if (pid > 0)
		fg_pid = pid;
	else
		prompt();
}

static void submit_line(void)
{
	term_putc('\n');
	if (line_len) {
		unsigned int i;
		for (i = 0; line[i] && i < sizeof(hist[hist_next]) - 1; i++)
			hist[hist_next][i] = line[i];
		hist[hist_next][i] = '\0';
		hist_next = (hist_next + 1) % HIST_MAX;
		if (hist_count < HIST_MAX)
			hist_count++;
	}
	hist_browse = 0;
	run_line();
}

static unsigned int name_len(const char *s, unsigned int max)
{
	unsigned int n = 0;

	while (n < max && s[n])
		n++;
	while (n && s[n - 1] == ' ')
		n--;
	return n;
}

static int prefix_is(const char *word, unsigned int wl, const char *name,
		     unsigned int nl)
{
	unsigned int i;

	if (wl > nl)
		return 0;
	for (i = 0; i < wl; i++)
		if (word[i] != name[i])
			return 0;
	return 1;
}

/* Complete the token at the end of the line.  A token after the command word
 * (or any token containing '/') is a path; the first token is a command. */
static void complete_line(void)
{
	unsigned int start = line_len;
	unsigned int tlen, slash = 0, i, n, bl, cl, matches = 0;
	char token[LINE_MAX], dir[VFS_PATH_MAX], base[LINE_MAX];
	char cand[LINE_MAX];
	int has_slash = 0, cand_dir = 0;

	while (start > 0 && line[start - 1] != ' ')
		start--;
	tlen = line_len - start;
	if (!tlen)
		return;
	for (i = 0; i < tlen && i + 1 < sizeof(token); i++)
		token[i] = line[start + i];
	token[i] = '\0';
	for (i = 0; i < tlen; i++)
		if (token[i] == '/') {
			has_slash = 1;
			slash = i;
		}

	if (has_slash || start > 0) {
		/* Path token: split into directory + basename prefix. */
		if (!has_slash) {
			dir[0] = '.';
			dir[1] = '\0';
			for (i = 0; i < tlen && i + 1 < sizeof(base); i++)
				base[i] = token[i];
			base[i] = '\0';
			bl = i;
		} else if (slash == 0) {
			dir[0] = '/';
			dir[1] = '\0';
			for (i = 0; i + 1 < tlen && i + 1 < sizeof(base); i++)
				base[i] = token[i + 1];
			base[i] = '\0';
			bl = i;
		} else {
			if (slash >= sizeof(dir))
				return;
			for (i = 0; i < slash; i++)
				dir[i] = token[i];
			dir[i] = '\0';
			for (i = 0; slash + 1 + i < tlen &&
				    i + 1 < sizeof(base); i++)
				base[i] = token[slash + 1 + i];
			base[i] = '\0';
			bl = i;
		}

		if (dir[0] == '/' && dir[1] == '\0') {
			struct sys_mount m;

			for (i = 0;; i++) {
				if (u_syscall2(SYS_LIST_MOUNTS, (int)i, (int)&m) != 1)
					break;
				n = name_len(m.name, sizeof(m.name));
				if (!prefix_is(base, bl, m.name, n))
					continue;
				matches++;
				if (matches == 1) {
					cl = 0;
					cand[cl++] = '/';
					for (n = 0; m.name[n] && cl + 1 < sizeof(cand); n++)
						cand[cl++] = m.name[n];
					cand[cl] = '\0';
					cand_dir = 1;
				}
			}
		} else {
			struct sys_dirent d;

			for (i = 0;; i++) {
				char disp[64];
				unsigned int dl;

				if (u_syscall3(SYS_LIST_DIR, (int)dir, (int)i,
					       (int)&d) != 1)
					break;
				if (d.raw_83)
					u_fat_name(d.name, disp, sizeof(disp));
				else {
					for (dl = 0; dl + 1 < sizeof(disp) &&
						      d.name[dl]; dl++)
						disp[dl] = d.name[dl];
					disp[dl] = '\0';
				}
				n = name_len(disp, sizeof(disp));
				if (n == 0 ||
				    (n == 1 && disp[0] == '.') ||
				    (n == 2 && disp[0] == '.' && disp[1] == '.'))
					continue;
				if (!prefix_is(base, bl, disp, n))
					continue;
				matches++;
				if (matches == 1) {
					cl = 0;
					if (slash > 0) {
						unsigned int k;
						for (k = 0; k < slash &&
							    cl + 1 < sizeof(cand); k++)
							cand[cl++] = token[k];
						if (cl && cand[cl - 1] != '/' &&
						    cl + 1 < sizeof(cand))
							cand[cl++] = '/';
					}
					for (n = 0; disp[n] &&
						    n < sizeof(disp) &&
						    cl + 1 < sizeof(cand); n++)
						cand[cl++] = disp[n];
					cand[cl] = '\0';
					cand_dir = d.is_dir ? 1 : 0;
				}
			}
		}
	} else {
		/* Command-name completion: builtins + /ramfs/bin. */
		static const char * const builtins[] = {
			"cd", "path", "help", "clear", "ps", "kill"
		};
		struct sys_dirent d;

		for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
			n = name_len(builtins[i], 16);
			if (!prefix_is(token, tlen, builtins[i], n))
				continue;
			matches++;
			if (matches == 1) {
				for (n = 0; builtins[i][n] && n + 1 < sizeof(cand); n++)
					cand[n] = builtins[i][n];
				cand[n] = '\0';
			}
		}
		for (i = 0;; i++) {
			if (u_syscall3(SYS_LIST_DIR, (int)"/ramfs/bin", (int)i,
				       (int)&d) != 1)
				break;
			n = name_len(d.name, sizeof(d.name));
			if (!prefix_is(token, tlen, d.name, n))
				continue;
			matches++;
			if (matches == 1) {
				for (n = 0; d.name[n] && n < sizeof(d.name) &&
					    n + 1 < sizeof(cand); n++)
					cand[n] = d.name[n];
				cand[n] = '\0';
			}
		}
		cand_dir = 0;
	}

	if (matches != 1)
		return;

	line_len = start;
	for (i = 0; cand[i] && line_len + 1 < LINE_MAX; i++)
		line[line_len++] = cand[i];
	if (cand_dir && line_len + 1 < LINE_MAX)
		line[line_len++] = '/';
	else if (line_len + 1 < LINE_MAX)
		line[line_len++] = ' ';
	line[line_len] = '\0';
	line_show();
}

static void handle_key(int k)
{
	if (k == 0x03) {
		if (fg_pid > 0) {
			u_kill((unsigned int)fg_pid);
			fg_pid = -1;
			term_puts("^C\n");
			prompt();
		} else {
			line_len = 0;
			line[0] = '\0';
			term_puts("^C\n");
			prompt();
		}
		return;
	}
	if (k == '\n' || k == '\r') {
		submit_line();
		return;
	}
	if (k == '\b') {
		if (line_len) {
			line_len--;
			line[line_len] = '\0';
			line_show();
		}
		return;
	}
	if (k == IO_KEY_UP) {
		if ((int)hist_browse < (int)hist_count) {
			hist_browse++;
			history_load(hist_browse);
		}
		return;
	}
	if (k == IO_KEY_DOWN) {
		if (hist_browse > 0) {
			hist_browse--;
			if (hist_browse == 0)
				line_set("");
			else
				history_load(hist_browse);
		}
		return;
	}
	if (k == '\t') {
		complete_line();
		return;
	}
	if (k < 0x20 || k > 0x7E)
		return;
	if (line_len + 1 < LINE_MAX) {
		hist_browse = 0;
		line[line_len++] = (char)k;
		line[line_len] = '\0';
		term_putc((char)k);
	}
}

void _start(const char *arg)
{
	unsigned int now;

	(void)arg;
	tty = u_tty_create();
	if (tty <= 0)
		u_exit(1);
	if (u_win_create(512, 512, "term", &wi) != 0)
		u_exit(1);
	usurf_init(&surf, wi.fb, wi.width, wi.height, wi.width * 2, 16);
	cols = wi.width / 8;
	rows = wi.height / 16;
	if (cols > MAX_COLS) cols = MAX_COLS;
	if (rows > MAX_ROWS) rows = MAX_ROWS;
	term_clear();
	u_getcwd(cwd, sizeof(cwd));
	{
		const char *def = "/ramfs/bin";
		unsigned int i;
		for (i = 0; def[i] && i + 1 < sizeof(path); i++)
			path[i] = def[i];
		path[i] = '\0';
	}
	prompt();
	render();

	for (;;) {
		struct win_info cur;
		int got = 0;

		if (u_win_info(&cur) == 0 &&
		    (cur.width != wi.width || cur.height != wi.height)) {
			if (u_win_ack_resize() == 0) {
				wi = cur;
				usurf_init(&surf, wi.fb, wi.width, wi.height,
					   wi.width * 2, 16);
				cols = wi.width / 8;
				rows = wi.height / 16;
				if (cols > MAX_COLS) cols = MAX_COLS;
				if (rows > MAX_ROWS) rows = MAX_ROWS;
				term_clear();
				if (fg_pid < 0)
					prompt();
				else {
					cx = cy = 0;
					line_row = line_col = 0;
				}
				dirty = 1;
			}
		}
		drain_tty();
		if (fg_pid > 0) {
			if (!u_alive((unsigned int)fg_pid)) {
				fg_pid = -1;
				drain_tty();
				prompt();
			} else {
				int k = u_key_poll();
				if (k == 0x03)
					handle_key(k);
			}
		} else {
			int k = u_win_key_wait(100);
			if (k < 0)
				u_exit(1);
			while (k) {
				handle_key(k);
				got = 1;
				k = u_key_poll();
			}
		}
		now = u_ticks_ms();
		if (now - last_blink >= 500) {
			last_blink = now;
			cursor_on = !cursor_on;
			dirty = 1;
		}
		render();
		u_sleep_ms(got ? 1 : 10);
	}
}
