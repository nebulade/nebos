/* I/O library: VGA text output and PS/2 keyboard. */

#include "io.h"

#define VGA_BASE ((volatile unsigned short *)0xB8000)
#define VGA_ATTR 0x0A
#define KB_DATA  0x60
#define KB_STATUS 0x64

static inline unsigned char inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void disable_hw_cursor(void)
{
	outb(0x3D4, 0x0A);
	outb(0x3D5, 0x20);
}

/* Scancode set 1 (XT) -> ASCII. 0 = ignore. */
static const unsigned char scancode_to_ascii[256] __attribute__((section(".rodata"))) = {
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
	[0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
	[0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
	[0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
	[0x28] = '\'', [0x29] = '`',
	[0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
	[0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
	[0x35] = '/', [0x39] = ' ',
};

static int shift_pressed;
static unsigned int cursor;
static unsigned short cursor_cell;
static unsigned char last_scancode;
static int scroll_occurred;

static char apply_shift(char c)
{
	if (!shift_pressed) return c;
	if (c >= 'a' && c <= 'z') return (char)(c - 32);
	switch ((unsigned char)c) {
		case '1': return '!';  case '2': return '@';  case '3': return '#';
		case '4': return '$';  case '5': return '%';  case '6': return '^';
		case '7': return '&';  case '8': return '*';  case '9': return '(';
		case '0': return ')';  case '-': return '_';  case '=': return '+';
		case '[': return '{';  case ']': return '}';  case ';': return ':';
		case '\'': return '"'; case '`': return '~';  case '\\': return '|';
		case ',': return '<';  case '.': return '>';  case '/': return '?';
		default: return c;
	}
}

static void hide_cursor_at(unsigned int pos)
{
	VGA_BASE[pos] = cursor_cell;
}

void io_init(void)
{
	disable_hw_cursor();
	io_clear_screen();
	cursor = IO_CONTENT_OFFSET;
	cursor_cell = (unsigned short)(VGA_ATTR << 8);
	io_drain_keyboard();
}

void io_clear_screen(void)
{
	unsigned int i;
	for (i = 0; i < IO_VGA_ROWS * IO_VGA_COLS; i++)
		VGA_BASE[i] = (unsigned short)(VGA_ATTR << 8);
}

int io_did_scroll(void)
{
	if (!scroll_occurred)
		return 0;
	scroll_occurred = 0;
	return 1;
}

void io_putchar(char c)
{
	if (c == '\n') {
		hide_cursor_at(cursor);
		cursor = (cursor / IO_VGA_COLS + 1) * IO_VGA_COLS;
		if (cursor >= IO_CONTENT_OFFSET + IO_CONTENT_SIZE) {
			unsigned int i;
			for (i = 0; i < (IO_CONTENT_ROWS - 1) * IO_VGA_COLS; i++)
				VGA_BASE[IO_CONTENT_OFFSET + i] = VGA_BASE[IO_CONTENT_OFFSET + i + IO_VGA_COLS];
			for (i = (IO_CONTENT_ROWS - 1) * IO_VGA_COLS; i < IO_CONTENT_SIZE; i++)
				VGA_BASE[IO_CONTENT_OFFSET + i] = (unsigned short)(VGA_ATTR << 8);
			cursor = IO_CONTENT_OFFSET + (IO_CONTENT_ROWS - 1) * IO_VGA_COLS;
			scroll_occurred = 1;
		}
		cursor_cell = (unsigned short)(VGA_ATTR << 8);
		return;
	}
	if (c == '\b') {
		if (cursor > IO_CONTENT_OFFSET) {
			hide_cursor_at(cursor);
			cursor--;
			cursor_cell = VGA_BASE[cursor];
		}
		return;
	}
	if (cursor < IO_CONTENT_OFFSET + IO_CONTENT_SIZE) {
		VGA_BASE[cursor] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)c);
		cursor++;
		cursor_cell = (unsigned short)(VGA_ATTR << 8);
	}
}

void io_putstr(const char *s)
{
	while (*s) io_putchar(*s++);
}

unsigned int io_get_cursor(void)
{
	return cursor;
}

#define CURSOR_BLOCK ((unsigned short)((VGA_ATTR << 8) | 0xDB))

void io_cursor_show_block(void)
{
	VGA_BASE[cursor] = CURSOR_BLOCK;
}

void io_cursor_hide_block(void)
{
	VGA_BASE[cursor] = cursor_cell;
}

void io_vga_write(unsigned int pos, unsigned short value)
{
	VGA_BASE[pos] = value;
}

void io_drain_keyboard(void)
{
	while (inb(KB_STATUS) & 1)
		(void)inb(KB_DATA);
}

char io_poll_key(void)
{
	if (!(inb(KB_STATUS) & 1))
		return 0;
	{
		unsigned char sc = inb(KB_DATA);
		if (sc & 0x80) {
			if (sc == 0xAA || sc == 0xB6) shift_pressed = 0;
			return 0;
		}
		if (sc == 0x2A || sc == 0x36) {
			shift_pressed = 1;
			return 0;
		}
		{
			char c = (char)scancode_to_ascii[sc];
			if (c) {
				last_scancode = sc;
				return apply_shift(c);
			}
		}
	}
	return 0;
}

unsigned char io_last_scancode(void)
{
	return last_scancode;
}
