/* Terminal: prompt, busy indicator, blinking cursor, echo loop. */

#include "io.h"
#include "terminal.h"
#include "timer.h"

static unsigned int busy_idx;
static unsigned int last_busy_tick;
static const unsigned char busy_chars[4] = { 0x2D, 0x5C, 0x7C, 0x2F };

static int cursor_visible;
static unsigned int last_cursor_tick;

static unsigned int puthex(unsigned int pos, unsigned char byte)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned short cell = (unsigned short)(0x0A << 8);
	io_vga_write(pos++, (unsigned short)(cell | hex[byte >> 4]));
	io_vga_write(pos++, (unsigned short)(cell | hex[byte & 0x0F]));
	return pos;
}

static void debug_line(unsigned char sc, char c)
{
	unsigned int i, pos = IO_DEBUG_ROW * IO_VGA_COLS;
	for (i = 0; i < 79; i++)
		io_vga_write(pos + i, (unsigned short)(0x0A << 8));
	pos = IO_DEBUG_ROW * IO_VGA_COLS;
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | 'S'));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | 'C'));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | ':'));
	pos = puthex(pos, sc);
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | ' '));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | 'C'));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | 'H'));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | ':'));
	pos = puthex(pos, (unsigned char)c);
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | ' '));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | '\''));
	io_vga_write(pos++, (unsigned short)((0x0A << 8) | (c >= 0x20 && c < 0x7F ? (unsigned char)c : '.')));
	io_vga_write(pos,   (unsigned short)((0x0A << 8) | '\''));
}

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
				debug_line(io_last_scancode(), c);
				io_putchar(c);
				if (c == '\n')
					io_putstr(prompt);
			} else {
				__asm__ volatile ("hlt");
			}
		}
	}
}
