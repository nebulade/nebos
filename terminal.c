/* Terminal: prompt, busy indicator, blinking cursor, echo loop. */

#include "io.h"
#include "terminal.h"
#include "timer.h"

static unsigned int busy_idx;
static unsigned int last_busy_tick;
static const unsigned char busy_chars[4] = { 0x2D, 0x5C, 0x7C, 0x2F };

static int cursor_visible;
static unsigned int last_cursor_tick;

static void update_busy(void)
{
	unsigned int t = get_ticks_ms();
	if (t - last_busy_tick >= BUSY_MS) {
		last_busy_tick = t;
		busy_idx = (busy_idx + 1) % 4;
	}
	io_vga_write(IO_BUSY_POS, (unsigned short)((0x0A << 8) | busy_chars[busy_idx]));
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
	last_cursor_tick = get_ticks_ms();
	last_busy_tick   = get_ticks_ms();
	cursor_visible   = 1;
	busy_idx         = 0;

	io_putstr(msg);
	io_putstr(prompt);

	for (;;) {
		update_busy();
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
