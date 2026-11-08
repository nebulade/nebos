/* I/O library: PS/2 keyboard and text output.
   Text output is routed to the current process's screen (see process.c);
   slot 0 / boot fall back to a static screen the WM does not render. */

#include "io.h"
#include "fbtext.h"
#include "mouse.h"
#include "process.h"
#include "screen.h"
#include "timer.h"
#include "tty.h"

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
static int ctrl_pressed;
static int kbd_extended;  /* 1 after 0xE0, next byte is extended scancode */

/* Software typematic repeat.  PS/2 keyboards repeat in hardware; the USB HID
 * backend only reports changes, so a held key would otherwise produce exactly
 * one event.  Track the last make code here (both backends end up in this
 * decoder) and re-deliver it after the usual delay/rate. */
#define KBD_REPEAT_DELAY_MS 500
#define KBD_REPEAT_RATE_MS  33

static int repeat_key;
static int repeat_active;
static unsigned int repeat_ext;
static unsigned int repeat_sc;
static unsigned int repeat_at;

#define KBD_BUF_SIZE 16
static volatile unsigned char kbd_buf[KBD_BUF_SIZE];
static volatile unsigned int kbd_head, kbd_tail;

/* Fallback screen for slot 0 / boot / exception output (not rendered by WM). */
static struct text_screen io_fallback;

static struct text_screen *io_current_screen(void)
{
	return &io_fallback;
}

/* IRQ1: read the controller status first and only read data when the output
 * buffer is full.  Some USB-legacy SMM implementations keep raising IRQ1:
 * reading 0x60 with no byte present does not acknowledge anything, and a
 * level-triggered line then storms.  The loop is bounded, and if the rate
 * gets absurd we mask IRQ1 and let the WM fall back to polling. */
static unsigned int kbd_win_start;
static unsigned int kbd_win_count;
static volatile int kbd_irq_masked;

int io_keyboard_irq_masked(void)
{
	return kbd_irq_masked;
}

void kbd_irq(void)
{
	unsigned int guard = 0;
	unsigned int now = get_ticks_ms();
	unsigned char st;

	if (now - kbd_win_start >= 1000) {
		kbd_win_start = now;
		kbd_win_count = 0;
	}
	kbd_win_count++;
	if (!kbd_irq_masked && kbd_win_count > 200) {
		/* IRQ1 storm; mask it (master PIC) and poll instead. */
		outb(0x21, (unsigned char)(inb(0x21) | 0x02));
		kbd_irq_masked = 1;
	}

	while (guard++ < 32) {
		st = inb(KB_STATUS);
		if (!(st & 0x01))
			break;
		if (st & 0x20) {
			/* AUX (mouse) byte: the IRQ12 handler or the mouse
			 * poll owns it while streaming is enabled.  With no
			 * owner, discard it so it cannot block the keyboard. */
			if (mouse_streaming_enabled())
				break;
			(void)inb(KB_DATA);
			continue;
		}
		{
			unsigned char sc = inb(KB_DATA);
			{
				unsigned int next = (kbd_head + 1) % KBD_BUF_SIZE;
				if (next != kbd_tail) {
					kbd_buf[kbd_head] = sc;
					kbd_head = next;
				}
			}
		}
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

/* A process with a tty writes there; the kernel terminal/boot fallback keeps
 * the old cell-screen path until the ring-3 terminal is running. */
static int io_tty(void)
{
	struct process *p = current_proc();
	int id = p ? p->tty : 0;

	return tty_valid(id) ? id : 0;
}



void io_putchar(char c)
{
	int id = io_tty();

	if (id) {
		tty_write(id, &c, 1);
		return;
	}
	screen_putchar(io_current_screen(), c);
}

void io_putstr(const char *s)
{
	int id = io_tty();

	if (id) {
		tty_write_str(id, s);
		return;
	}
	screen_putstr(io_current_screen(), s);
}

void io_puthex(unsigned int n)
{
	if (io_tty()) {
		static const char hex[] = "0123456789ABCDEF";
		char buf[9];
		unsigned int i = 8;

		buf[8] = '\0';
		do {
			buf[--i] = hex[n & 0xF];
			n >>= 4;
		} while (i > 0);
		io_putstr(buf);
		return;
	}
	screen_puthex(io_current_screen(), n);
}






void io_drain_keyboard(void)
{
	while (inb(KB_STATUS) & 1)
		(void)inb(KB_DATA);
	kbd_head = kbd_tail = 0;
	kbd_extended = 0;
	repeat_active = 0;
}

static void kbd_repeat_start(int key, unsigned int ext, unsigned int sc)
{
	repeat_key = key;
	repeat_ext = ext;
	repeat_sc = sc;
	repeat_at = get_ticks_ms() + KBD_REPEAT_DELAY_MS;
	repeat_active = 1;
}

static void kbd_repeat_stop(unsigned int ext, unsigned int sc)
{
	if (repeat_active && repeat_ext == ext && repeat_sc == sc)
		repeat_active = 0;
}

/* Return a synthetic repeat event when the held key is due.  Called only when
 * the real scancode queue is empty, so a pending break code is decoded first
 * and cancels the repeat before it can leak through. */
static int kbd_repeat_poll(void)
{
	unsigned int now;

	if (!repeat_active)
		return 0;
	now = get_ticks_ms();
	if (now - repeat_at > 0x7FFFFFFFu)   /* signed deadline comparison */
		return 0;
	repeat_at = now + KBD_REPEAT_RATE_MS;
	return repeat_key;
}

/* Process one scancode byte; shared by io_poll_key and io_poll_key_with_hw. */
static int kbd_process_byte(unsigned char sc)
{
	int key;

	if (sc == 0xE0) {
		kbd_extended = 1;
		return 0;
	}
	if (kbd_extended) {
		kbd_extended = 0;
		if (sc & 0x80) {
			kbd_repeat_stop(1, sc & 0x7F);
			if (sc == 0x9D)   /* right Ctrl release */
				ctrl_pressed = 0;
			return 0;  /* key release */
		}
		if (sc == 0x1D) {         /* right Ctrl */
			ctrl_pressed = 1;
			return 0;
		}
		if (sc == 0x4B)
			key = ctrl_pressed ? IO_KEY_CTRL_LEFT : IO_KEY_LEFT;
		else if (sc == 0x4D)
			key = ctrl_pressed ? IO_KEY_CTRL_RIGHT : IO_KEY_RIGHT;
		else if (sc == 0x48)
			key = IO_KEY_UP;
		else if (sc == 0x50)
			key = IO_KEY_DOWN;
		else if (sc == 0x49)
			key = IO_KEY_PAGE_UP;
		else if (sc == 0x51)
			key = IO_KEY_PAGE_DOWN;
		else if (sc == 0x47)
			key = IO_KEY_HOME;
		else if (sc == 0x4F)
			key = IO_KEY_END;
		else if (sc == 0x53)
			key = ctrl_pressed ? IO_KEY_CTRL_DELETE : IO_KEY_DELETE;
		else
			return 0;
		kbd_repeat_start(key, 1, sc);
		return key;
	}
	if (sc & 0x80) {
		if (sc == 0xAA || sc == 0xB6) shift_pressed = 0;
		if (sc == 0x9D) ctrl_pressed = 0;
		kbd_repeat_stop(0, sc & 0x7F);
		return 0;
	}
	if (sc == 0x2A || sc == 0x36) {
		shift_pressed = 1;
		return 0;
	}
	if (sc == 0x1D) {
		ctrl_pressed = 1;
		return 0;
	}
	if (sc == 0x0E && ctrl_pressed) {
		/* Ctrl+Backspace: delete the previous word. */
		kbd_repeat_start(IO_KEY_CTRL_BACKSPACE, 0, sc);
		return IO_KEY_CTRL_BACKSPACE;
	}
	{
		char c = (char)scancode_to_ascii[sc];
		if (c) {
			c = apply_shift(c);
			if (ctrl_pressed) {
				/* Ctrl+letter -> control character (e.g. Ctrl+C -> 0x03). */
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
					key = (unsigned char)(c & 0x1F);
				else
					return 0;
			} else {
				key = (unsigned char)c;
			}
			kbd_repeat_start(key, 0, sc);
			return key;
		}
	}
	return 0;
}

int io_poll_key(void)
{
	unsigned char sc;

	if (!kbd_buf_get(&sc))
		return kbd_repeat_poll();
	return kbd_process_byte(sc);
}

/* Like io_poll_key but also reads from port 0x60 when buffer is empty.
   For use by full-screen programs (e.g. space.bin) where IRQ may be missed on
   real HW.  Only reads when a keyboard byte is actually pending (OBF set, AUX
   clear); the status+data pair is read atomically wrt IRQ1. */
int io_poll_key_with_hw(void)
{
	unsigned char sc = 0;
	unsigned int flags;
	int got = 0;

	if (kbd_tail != kbd_head) {
		if (!kbd_buf_get(&sc))
			return 0;
		return kbd_process_byte(sc);
	}

	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	{
		unsigned char st = inb(KB_STATUS);
		if ((st & 0x01) && !(st & 0x20)) {
			sc = inb(KB_DATA);
			got = 1;
		}
	}
	__asm__ volatile ("pushl %0; popfl" : : "r"(flags));

	if (!got)
		return kbd_repeat_poll();
	return kbd_process_byte(sc);
}

/* Queue a scancode from a non-IRQ source (the USB keyboard backend); the
   normal io_poll_key/io_poll_key_with_hw path then decodes it.  Interrupts
   are masked while touching the ring so an IRQ1 producer cannot corrupt it. */
void io_inject_scancode(unsigned char sc)
{
	unsigned int flags;

	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	{
		unsigned int next = (kbd_head + 1) % KBD_BUF_SIZE;
		if (next != kbd_tail) {
			kbd_buf[kbd_head] = sc;
			kbd_head = next;
		}
	}
	__asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

static int kbd_raw;

void io_set_raw_keyboard(int on)
{
	kbd_raw = on;
}

int io_keyboard_raw(void)
{
	return kbd_raw;
}
