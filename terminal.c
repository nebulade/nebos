/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch. */

#include "io.h"
#include "fbtext.h"
#include "rtc.h"
#include "terminal.h"
#include "timer.h"

extern void program_hello_main(void);
extern void program_reboot_main(void);
extern void program_ls_main(void);
extern void program_cat_main(const char *filename);
extern void program_run_main(const char *filename);
extern void program_clock_main(void);
extern void program_colors_main(void);
extern void program_display_main(void);
extern void disk_set_drive(unsigned int drive);
extern unsigned int disk_get_drive(void);

#define DATETIME_MS 1000
#define LINE_MAX 64
#define HISTORY_SIZE 32
static unsigned int last_datetime_tick;
static char datetime_buf[IO_DATETIME_LEN + 1];
static char history[HISTORY_SIZE][LINE_MAX];
static unsigned int history_count;
static unsigned int history_next;

static int cursor_visible;
static unsigned int last_cursor_tick;

static void redraw_datetime(void)
{
	rtc_read_datetime(datetime_buf);
	{
		unsigned int i;
		for (i = 0; i < IO_DATETIME_LEN && datetime_buf[i]; i++)
			io_vga_write(IO_DATETIME_POS + i,
				(unsigned short)((0x0A << 8) | (unsigned char)datetime_buf[i]));
	}
}

static void update_datetime(void)
{
	unsigned int t = get_ticks_ms();
	if (t - last_datetime_tick >= DATETIME_MS) {
		last_datetime_tick = t;
		redraw_datetime();
	}
}

static void update_cursor_blink(void)
{
	unsigned int t = get_ticks_ms();
	if (t - last_cursor_tick >= CURSOR_MS) {
		last_cursor_tick = t;
		cursor_visible = !cursor_visible;
	}
	if (cursor_visible)
		io_cursor_show_block();
	else
		io_cursor_hide_block();
}

static const char banner[] __attribute__((section(".text"))) = "Nebs OS |o|";
#define BANNER_LEN 12
static const char prompt[] __attribute__((section(".text"))) = "\n> ";
static const char prompt_first[] __attribute__((section(".text"))) = "> ";

/* Draw row 0: banner left, datetime right. Call when terminal is active. */
static void draw_header_row(void)
{
	unsigned int i;
	for (i = 0; i < BANNER_LEN && banner[i]; i++)
		io_vga_write(i, (unsigned short)((0x0A << 8) | (unsigned char)banner[i]));
	for (; i < IO_DATETIME_POS; i++)
		io_vga_write(i, (unsigned short)(0x0A << 8));
	redraw_datetime();
	fbtext_draw_rainbow_separator();
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

static int is_cmd_ls(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's')
		return 0;
	i += 2;
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

static int is_cmd_drive(const char *line, unsigned int line_len, unsigned int *drive_num)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'r' || line[i+2] != 'i' || line[i+3] != 'v' || line[i+4] != 'e')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len || line[i] < '0' || line[i] > '1')
		return 0;
	*drive_num = (unsigned int)(line[i] - '0');
	i++;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
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

/* Copy history entry (1 = newest) into line, set line_len/line_pos, redraw from start. */
static void history_load_into_line(char *line, unsigned int *line_len, unsigned int *line_pos,
	unsigned int line_start, unsigned int browse)
{
	unsigned int slot, len, old_len;
	const char *src;
	if (browse < 1 || browse > history_count)
		return;
	old_len = *line_len;
	slot = (history_next + HISTORY_SIZE - browse) % HISTORY_SIZE;
	src = history[slot];
	for (len = 0; len < LINE_MAX - 1 && src[len]; len++)
		line[len] = src[len];
	line[len] = '\0';
	*line_len = len;
	*line_pos = len;
	redraw_line_clear(line, len, len, line_start, old_len > len ? old_len : len);
}

/* Push current line to history if non-empty (and not duplicate of newest). */
static void history_push(const char *line, unsigned int line_len)
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
	if (history_count > 0) {
		slot = (history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
		for (i = 0; i < line_len && i < LINE_MAX - 1 && history[slot][i]; i++) {
			if (history[slot][i] != line[i])
				break;
		}
		if (i == line_len && history[slot][i] == '\0')
			return;  /* duplicate of newest */
	}
	slot = history_next;
	for (i = 0; i < line_len && i < LINE_MAX - 1; i++)
		history[slot][i] = line[i];
	history[slot][i] = '\0';
	history_next = (history_next + 1) % HISTORY_SIZE;
	if (history_count < HISTORY_SIZE)
		history_count++;
}

void terminal_run(void)
{
	static char line[LINE_MAX];
	static unsigned int line_len;
	static unsigned int line_pos;   /* cursor index in line, 0..line_len */
	static unsigned int line_start; /* VGA position of first char of line */
	static unsigned int history_browse; /* 0 = not browsing, 1 = newest, 2 = older, ... */

	last_cursor_tick   = get_ticks_ms();
	last_datetime_tick = get_ticks_ms();
	cursor_visible     = 1;
	line_len           = 0;
	line_pos           = 0;
	line[0]            = '\0';
	history_browse     = 0;

	io_drain_keyboard();
	draw_header_row();
	io_putstr(prompt_first);
	line_start = io_get_cursor();

	for (;;) {
		update_datetime();
		update_cursor_blink();
		{
			int k = io_poll_key();
			if (k) {
				if (k == IO_KEY_LEFT) {
					if (line_pos > 0) {
						line_pos--;
						io_set_cursor(line_start + line_pos);
					}
				} else if (k == IO_KEY_RIGHT) {
					if (line_pos < line_len) {
						line_pos++;
						io_set_cursor(line_start + line_pos);
					}
				} else if (k == IO_KEY_UP) {
					if (history_count > 0) {
						if (history_browse < history_count)
							history_browse++;
						history_load_into_line(line, &line_len, &line_pos, line_start, history_browse);
					}
				} else if (k == IO_KEY_DOWN) {
					if (history_browse > 0) {
						history_browse--;
						if (history_browse == 0) {
							{
								unsigned int clear_len = line_len;
								line_len = 0;
								line_pos = 0;
								line[0]  = '\0';
								redraw_line_clear(line, 0, 0, line_start, clear_len);
							}
						} else {
							history_load_into_line(line, &line_len, &line_pos, line_start, history_browse);
						}
					}
				} else if (k == '\b') {
					if (line_pos > 0) {
						unsigned int i;
						line_pos--;
						for (i = line_pos; i < line_len; i++)
							line[i] = line[i + 1];
						line_len--;
						line[line_len] = '\0';
						redraw_line_from(line, line_len, line_pos, line_start, line_pos);
					}
				} else if (k == '\n') {
					static char cat_fname[CAT_FNAME_MAX];
					unsigned int drive_sel;
					int skip_prompt = 0;
					io_putchar('\n');
					history_push(line, line_len);
					history_browse = 0;
					if (is_cmd_hello(line, line_len)) {
						program_hello_main();
						io_drain_keyboard();
						draw_header_row();
					} else if (is_cmd_reboot(line, line_len)) {
						program_reboot_main();
					} else if (is_cmd_ls(line, line_len)) {
						program_ls_main();
						io_drain_keyboard();
						draw_header_row();
					} else if (is_cmd_clock(line, line_len)) {
						program_clock_main();
						io_drain_keyboard();
						io_clear_screen();
						draw_header_row();
					} else if (is_cmd_colors(line, line_len)) {
						program_colors_main();
						io_drain_keyboard();
						io_clear_screen();
						draw_header_row();
					} else if (is_cmd_cat(line, line_len, cat_fname, CAT_FNAME_MAX)) {
						program_cat_main(cat_fname);
						io_drain_keyboard();
						draw_header_row();
					} else if (is_cmd_run(line, line_len, cat_fname, CAT_FNAME_MAX)) {
						program_run_main(cat_fname);
						io_drain_keyboard();
						draw_header_row();
					} else if (is_cmd_space(line, line_len)) {
						program_run_main("space.bin");
						io_drain_keyboard();
						draw_header_row();
					} else if (is_cmd_display(line, line_len)) {
						program_display_main();
					} else if (is_cmd_drive(line, line_len, &drive_sel)) {
						disk_set_drive(drive_sel);
						io_putstr("drive ");
						io_putchar((char)('0' + disk_get_drive()));
						io_putstr(" selected\n");
					} else if (is_cmd_help(line, line_len)) {
						io_putstr("hello\n");
						io_putstr("reboot\n");
						io_putstr("ls\n");
						io_putstr("clock\n");
						io_putstr("colors\n");
						io_putstr("cat <file>\n");
						io_putstr("run <file>\n");
						io_putstr("space\n");
						io_putstr("display\n");
						io_putstr("drive <0|1>\n");
						io_putstr("help\n");
						io_putstr("clear\n");
					} else if (is_cmd_clear(line, line_len)) {
						io_clear_screen();
						draw_header_row();
						io_set_cursor(IO_CONTENT_OFFSET);
						io_putstr(prompt_first);
						line_start = io_get_cursor();
						skip_prompt = 1;
					} else {
						unsigned int i = 0;
						while (i < line_len && line[i] == ' ')
							i++;
						if (i < line_len)
							io_putstr("command not found\n");
					}
					line_len  = 0;
					line_pos  = 0;
					line[0]   = '\0';
					if (!skip_prompt) {
						io_putstr(prompt);
						line_start = io_get_cursor();
					}
					if (io_did_scroll())
						redraw_datetime();
				} else {
					{
						unsigned char u = (unsigned char)k;
						if (u >= 0x20 && u < 0x7F && line_len < LINE_MAX - 1) {
							unsigned int i;
							history_browse = 0;
							for (i = line_len; i > line_pos; i--)
								line[i] = line[i - 1];
							line[line_pos] = (char)k;
							line_len++;
							line[line_len] = '\0';
							line_pos++;
							redraw_line_from(line, line_len, line_pos, line_start, line_pos - 1);
						}
					}
				}
			} else {
				__asm__ volatile ("hlt");
			}
		}
	}
}
