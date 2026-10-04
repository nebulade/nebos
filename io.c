/* I/O library: PS/2 keyboard and text output.
   Text output is routed to the current process's screen (see process.c);
   slot 0 / boot fall back to a static screen the WM does not render. */

#include "io.h"
#include "fbtext.h"
#include "process.h"
#include "screen.h"

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
static unsigned char last_scancode;
static int kbd_extended;  /* 1 after 0xE0, next byte is extended scancode */

#define KBD_BUF_SIZE 16
static volatile unsigned char kbd_buf[KBD_BUF_SIZE];
static volatile unsigned int kbd_head, kbd_tail;

/* Fallback screen for slot 0 / boot / exception output (not rendered by WM). */
static struct text_screen io_fallback;

static struct text_screen *io_current_screen(void)
{
	struct text_screen *s = procs[current_pid].screen;
	return s ? s : &io_fallback;
}

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
	screen_init(&io_fallback);
	io_drain_keyboard();
}

void io_clear_screen(void)
{
	screen_clear(io_current_screen());
}

int io_did_scroll(void)
{
	return screen_did_scroll(io_current_screen());
}

void io_putchar(char c)
{
	screen_putchar(io_current_screen(), c);
}

void io_putstr(const char *s)
{
	screen_putstr(io_current_screen(), s);
}

void io_puthex(unsigned int n)
{
	screen_puthex(io_current_screen(), n);
}

unsigned int io_get_cursor(void)
{
	return screen_get_cursor(io_current_screen());
}

void io_set_cursor(unsigned int pos)
{
	screen_set_cursor(io_current_screen(), pos);
}

void io_cursor_show_block(void)
{
	struct text_screen *s = io_current_screen();
	s->cursor_visible = 1;
	s->dirty = 1;
}

void io_cursor_hide_block(void)
{
	struct text_screen *s = io_current_screen();
	s->cursor_visible = 0;
	s->dirty = 1;
}

void io_vga_write(unsigned int pos, unsigned short value)
{
	screen_put_at(io_current_screen(), pos, value);
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
		if (sc == 0x48) return IO_KEY_UP;
		if (sc == 0x50) return IO_KEY_DOWN;
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
