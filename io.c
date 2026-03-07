/* I/O library: framebuffer text output and PS/2 keyboard. */

#include "io.h"
#include "fbtext.h"

#define VGA_ATTR 0x0A

static unsigned int io_cols = 80;
static unsigned int io_rows = 25;
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

/* Scancode set 1 (XT) -> ASCII. 0 = ignore. */
static const unsigned char scancode_to_ascii[256] __attribute__((section(".rodata"))) = {
	[0x01] = 0x1B,  /* Escape */
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
static int kbd_extended;  /* 1 after 0xE0, next byte is extended scancode */

#define KBD_BUF_SIZE 16
static volatile unsigned char kbd_buf[KBD_BUF_SIZE];
static volatile unsigned int kbd_head, kbd_tail;

void kbd_irq(void)
{
	unsigned char sc = inb(KB_DATA);
	unsigned int next = (kbd_head + 1) % KBD_BUF_SIZE;
	if (next != kbd_tail) {
		kbd_buf[kbd_head] = sc;
		kbd_head = next;
	}
	outb(0x20, 0x20);
}

static int kbd_buf_get(unsigned char *sc)
{
	if (kbd_tail == kbd_head)
		return 0;
	*sc = kbd_buf[kbd_tail];
	kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
	return 1;
}

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
	unsigned int col = pos % io_cols;
	unsigned int row = pos / io_cols;
	fbtext_set_cursor(col, row);
	fbtext_hide_cursor((unsigned char)(cursor_cell & 0xFF), (unsigned char)(cursor_cell >> 8));
}

unsigned int io_get_cols(void)
{
	return io_cols;
}

unsigned int io_get_rows(void)
{
	return io_rows;
}

void io_init(void)
{
	fbtext_init();
	io_cols = fbtext_get_cols();
	io_rows = fbtext_get_rows();
	io_clear_screen();
	cursor = IO_CONTENT_OFFSET;
	cursor_cell = (unsigned short)(VGA_ATTR << 8);
	io_drain_keyboard();
}

void io_clear_screen(void)
{
	fbtext_clear();
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
	unsigned int ncols = io_cols;
	unsigned int content_off = ncols;
	unsigned int content_size = (io_rows - 1) * ncols;

	if (c == '\n') {
		hide_cursor_at(cursor);
		cursor = (cursor / ncols + 1) * ncols;
		if (cursor >= content_off + content_size) {
			fbtext_scroll_up_one_line();
			cursor = content_off + (io_rows - 2) * ncols;
			scroll_occurred = 1;
		}
		cursor_cell = (unsigned short)(VGA_ATTR << 8);
		return;
	}
	if (c == '\b') {
		if (cursor > content_off) {
			hide_cursor_at(cursor);
			cursor--;
			{
				unsigned int col = cursor % ncols;
				unsigned int row = cursor / ncols;
				fbtext_put_char_at(col, row, ' ', VGA_ATTR);
			}
			cursor_cell = (unsigned short)((VGA_ATTR << 8) | ' ');
		}
		return;
	}
	if (cursor < content_off + content_size) {
		{
			unsigned int col = cursor % ncols;
			unsigned int row = cursor / ncols;
			fbtext_put_char_at(col, row, (unsigned char)c, VGA_ATTR);
		}
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

void io_set_cursor(unsigned int pos)
{
	if (pos >= io_rows * io_cols)
		return;
	hide_cursor_at(cursor);
	cursor = pos;
	cursor_cell = (unsigned short)(VGA_ATTR << 8);
	fbtext_set_cursor(cursor % io_cols, cursor / io_cols);
}

void io_cursor_show_block(void)
{
	fbtext_set_cursor(cursor % io_cols, cursor / io_cols);
	fbtext_show_cursor();
}

void io_cursor_hide_block(void)
{
	fbtext_hide_cursor((unsigned char)(cursor_cell & 0xFF), (unsigned char)(cursor_cell >> 8));
}

void io_vga_write(unsigned int pos, unsigned short value)
{
	unsigned int col = pos % io_cols;
	unsigned int row = pos / io_cols;
	fbtext_put_char_at(col, row, (unsigned char)(value & 0xFF), (unsigned char)(value >> 8));
}

void io_drain_keyboard(void)
{
	while (inb(KB_STATUS) & 1)
		(void)inb(KB_DATA);
	kbd_head = kbd_tail = 0;
	kbd_extended = 0;
}

/* Process one scancode byte; shared by io_poll_key and io_poll_key_with_hw. */
static int kbd_process_byte(unsigned char sc)
{
	if (sc == 0xE0) {
		kbd_extended = 1;
		return 0;
	}
	if (kbd_extended) {
		kbd_extended = 0;
		if (sc & 0x80)
			return 0;  /* key release */
		if (sc == 0x4B) return IO_KEY_LEFT;
		if (sc == 0x4D) return IO_KEY_RIGHT;
		return 0;
	}
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
			return (unsigned char)apply_shift(c);
		}
	}
	return 0;
}

int io_poll_key(void)
{
	unsigned char sc;
	if (!kbd_buf_get(&sc))
		return 0;
	return kbd_process_byte(sc);
}

/* Like io_poll_key but also reads from port 0x60 when buffer is empty.
   For use by full-screen programs (e.g. space.bin) where IRQ may be missed on real HW. */
int io_poll_key_with_hw(void)
{
	unsigned char sc;
	if (kbd_tail == kbd_head) {
		if (!(inb(KB_STATUS) & 1))
			return 0;
		sc = inb(KB_DATA);
	} else {
		if (!kbd_buf_get(&sc))
			return 0;
	}
	return kbd_process_byte(sc);
}

unsigned char io_last_scancode(void)
{
	return last_scancode;
}
