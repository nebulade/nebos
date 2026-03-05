/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch. */

#include "io.h"
#include "rtc.h"
#include "terminal.h"
#include "timer.h"

extern void program_hello_main(void);

#define DATETIME_MS 1000
#define LINE_MAX 64
static unsigned int last_datetime_tick;
static char datetime_buf[IO_DATETIME_LEN + 1];

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

void terminal_run(void)
{
	static char line[LINE_MAX];
	static unsigned int line_len;

	last_cursor_tick   = get_ticks_ms();
	last_datetime_tick = get_ticks_ms();
	cursor_visible     = 1;
	line_len           = 0;
	line[0]            = '\0';

	io_drain_keyboard();
	draw_header_row();
	io_putstr(prompt_first);

	for (;;) {
		update_datetime();
		update_cursor_blink();
		{
			char c = io_poll_key();
			if (c) {
				if (c == '\b') {
					if (line_len > 0) {
						line_len--;
						line[line_len] = '\0';
						io_putchar(c);
					}
				} else if (c == '\n') {
					io_putchar(c);
					if (is_cmd_hello(line, line_len)) {
						program_hello_main();
						io_drain_keyboard();
						draw_header_row();
					}
					line_len = 0;
					line[0]  = '\0';
					io_putstr(prompt);
					if (io_did_scroll())
						redraw_datetime();
				} else {
					{
						unsigned char u = (unsigned char)c;
						if (u >= 0x20 && u < 0x7F && line_len < LINE_MAX - 1) {
							line[line_len++] = c;
							line[line_len]   = '\0';
						}
					}
					io_putchar(c);
				}
			} else {
				__asm__ volatile ("hlt");
			}
		}
	}
}
