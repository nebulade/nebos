/* Terminal: prompt, datetime (top right), blinking cursor, echo loop. */

#include "io.h"
#include "rtc.h"
#include "terminal.h"
#include "timer.h"

#define DATETIME_MS 1000
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

static const char msg[] __attribute__((section(".text"))) = "Nebs OS |o|";
static const char prompt[] __attribute__((section(".text"))) = "\n> ";

void terminal_run(void)
{
	last_cursor_tick   = get_ticks_ms();
	last_datetime_tick = get_ticks_ms();
	cursor_visible     = 1;

	io_putstr(msg);
	io_putstr(prompt);

	for (;;) {
		update_datetime();
		update_cursor_blink();
		{
			char c = io_poll_key();
			if (c) {
				io_putchar(c);
				if (c == '\n') {
					io_putstr(prompt);
					if (io_did_scroll())
						redraw_datetime();
				}
			} else {
				__asm__ volatile ("hlt");
			}
		}
	}
}
