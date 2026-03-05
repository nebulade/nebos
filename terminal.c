/* Terminal: prompt, blinking cursor, echo loop. */

#include "io.h"
#include "terminal.h"
#include "timer.h"

static int cursor_visible;
static unsigned int last_cursor_tick;

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
	last_cursor_tick = get_ticks_ms();
	cursor_visible   = 1;

	io_putstr(msg);
	io_putstr(prompt);

	for (;;) {
		update_cursor_blink();
		{
			char c = io_poll_key();
			if (c) {
				io_putchar(c);
				if (c == '\n')
					io_putstr(prompt);
			} else {
				__asm__ volatile ("hlt");
			}
		}
	}
}
