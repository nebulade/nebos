/* Kernel: entered from bootloader in 32-bit protected mode at 0x7E00.
 * Must be linked so this function is at 0x7E00 (see kernel.ld).
 * Simple terminal: keyboard echo. */

#define VGA_BASE ((volatile unsigned short *)0xB8000)
#define VGA_ATTR 0x0A  /* green on black */
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_SIZE (VGA_COLS * VGA_ROWS)
#define DEBUG_ROW 24                    /* last row: debug line (input vs echo) */
#define CONTENT_ROWS (DEBUG_ROW)        /* rows 0..23 for terminal */
#define CONTENT_SIZE (CONTENT_ROWS * VGA_COLS)

/* Port I/O */
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

/* PS/2 keyboard: data port 0x60, status 0x64 (bit 0 = output buffer full) */
#define KB_DATA  0x60
#define KB_STATUS 0x64

/* Timer: PIT at 1 kHz (1 tick = 1 ms). IRQ0 -> INT 0x20 after PIC remap. */
#define PIT_HZ      1000
#define CURSOR_MS   500
#define BUSY_MS     33

volatile unsigned int ticks;  /* milliseconds since timer_init */

/* IDT entry (8 bytes) and table for vector 0x20 (IRQ0). */
struct idt_entry {
	unsigned short offset_lo;
	unsigned short selector;
	unsigned char  zero;
	unsigned char  type;
	unsigned short offset_hi;
} __attribute__((packed));

struct idtr_desc {
	unsigned short limit;
	unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr_desc idtr;

extern void pit_irq_stub(void);

void pit_irq(void);  /* called from timer.S stub */

void pit_irq(void)
{
	ticks++;
	outb(0x20, 0x20);  /* EOI to master PIC */
}

static void timer_init(void)
{
	unsigned int stub = (unsigned int)pit_irq_stub;

	/* Remap PIC: IRQ0..7 -> INT 0x20..0x27 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x20);
	outb(0xA1, 0x28);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFE);  /* mask all except IRQ0 on master */
	outb(0xA1, 0xFF);  /* mask all on slave */

	/* IDT entry for 0x20 (IRQ0) */
	idt[0x20].offset_lo = (unsigned short)(stub & 0xFFFF);
	idt[0x20].selector  = 0x08;
	idt[0x20].zero     = 0;
	idt[0x20].type     = 0x8E;  /* 32-bit interrupt gate */
	idt[0x20].offset_hi = (unsigned short)((stub >> 16) & 0xFFFF);

	idtr.limit = (unsigned short)(sizeof(idt) - 1);
	idtr.base  = (unsigned int)idt;
	__asm__ volatile ("lidt %0" : : "m"(idtr));

	/* PIT channel 0: 1 kHz (divisor 1193) */
	outb(0x43, 0x36);
	outb(0x40, (unsigned char)(1193 & 0xFF));
	outb(0x40, (unsigned char)((1193 >> 8) & 0xFF));

	__asm__ volatile ("sti");
}

/* Scancode set 1 (XT) -> ASCII. 0 = ignore. Set 1 break = make | 0x80, we ignore those. */
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

/* Set 1: left shift make 0x2A break 0xAA, right shift make 0x36 break 0xB6 */
static int shift_pressed;

static unsigned int cursor;  /* linear position 0 .. CONTENT_SIZE-1 */
static unsigned char last_scancode;     /* set by getchar for debug line */
#define BUSY_POS (DEBUG_ROW * VGA_COLS + 79)  /* bottom-right: never overwritten by echo */
static unsigned int busy_idx;   /* 0..3 for busy_chars */
static unsigned int last_busy_tick;
/* Explicit ASCII so no encoding/escape issues: - \ | / */
static const unsigned char busy_chars[4] = { 0x2D, 0x5C, 0x7C, 0x2F };

/* Blinking cursor: cell content when cursor is hidden; 0xDB = full block (CP437) */
static unsigned short cursor_cell;
static int cursor_visible;
static unsigned int last_cursor_tick;
#define CURSOR_BLOCK ((unsigned short)((VGA_ATTR << 8) | 0xDB))

/* Restore cell at current cursor, then update cursor position and cursor_cell. */
static void hide_cursor_at(unsigned int pos)
{
	VGA_BASE[pos] = cursor_cell;
}

static void putchar(char c)
{
	if (c == '\n') {
		hide_cursor_at(cursor);
		cursor = (cursor / VGA_COLS + 1) * VGA_COLS;
		if (cursor >= CONTENT_SIZE) {
			unsigned int i;
			for (i = 0; i < (CONTENT_ROWS - 1) * VGA_COLS; i++)
				VGA_BASE[i] = VGA_BASE[i + VGA_COLS];
			for (i = (CONTENT_ROWS - 1) * VGA_COLS; i < CONTENT_SIZE; i++)
				VGA_BASE[i] = (unsigned short)(VGA_ATTR << 8);
			cursor = (CONTENT_ROWS - 1) * VGA_COLS;
		}
		cursor_cell = (unsigned short)(VGA_ATTR << 8);
		return;
	}
	if (c == '\b') {
		if (cursor > 0) {
			hide_cursor_at(cursor);
			cursor--;
			cursor_cell = VGA_BASE[cursor];
		}
		return;
	}
	if (cursor < CONTENT_SIZE) {
		VGA_BASE[cursor] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)c);
		cursor++;
		cursor_cell = (unsigned short)(VGA_ATTR << 8);
	}
}

/* Draw blinking cursor at current cursor position (every CURSOR_MS ms). */
static void update_cursor_blink(void)
{
	unsigned int t = ticks;
	if (t - last_cursor_tick >= CURSOR_MS) {
		last_cursor_tick = t;
		cursor_visible = !cursor_visible;
	}
	if (cursor_visible)
		VGA_BASE[cursor] = CURSOR_BLOCK;
	else
		VGA_BASE[cursor] = cursor_cell;
}

/* Write one hex byte at VGA position; returns next position. */
static unsigned int puthex(unsigned int pos, unsigned char byte)
{
	static const char hex[] = "0123456789ABCDEF";
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | hex[byte >> 4]);
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | hex[byte & 0x0F]);
	return pos;
}

/* Update bottom line with last scancode and decoded char. Leaves col 79 for busy. */
static void debug_line(unsigned char sc, char c)
{
	unsigned int i, pos = DEBUG_ROW * VGA_COLS;
	for (i = 0; i < 79; i++)
		VGA_BASE[pos + i] = (unsigned short)(VGA_ATTR << 8);
	/* "SC:XX CH:YY 'c'" */
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | 'S');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | 'C');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | ':');
	pos = puthex(pos, sc);
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | ' ');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | 'C');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | 'H');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | ':');
	pos = puthex(pos, (unsigned char)c);
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | ' ');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | '\'');
	VGA_BASE[pos++] = (unsigned short)((VGA_ATTR << 8) | (c >= 0x20 && c < 0x7F ? (unsigned char)c : '.'));
	VGA_BASE[pos]   = (unsigned short)((VGA_ATTR << 8) | '\'');
}

static void putstr(const char *s)
{
	while (*s) putchar(*s++);
}

/* Apply shift to get US layout shifted character. Returns c unchanged if no shift variant. */
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

/* Draw busy indicator at bottom-right (every BUSY_MS ms). */
static void update_busy(void)
{
	unsigned int t = ticks;
	if (t - last_busy_tick >= BUSY_MS) {
		last_busy_tick = t;
		busy_idx = (busy_idx + 1) % 4;
	}
	VGA_BASE[BUSY_POS] = (unsigned short)((VGA_ATTR << 8) | busy_chars[busy_idx]);
}

/* String in .text so it loads with the kernel */
static const char msg[] __attribute__((section(".text"))) = "Hello from C (protected mode)";
static const char prompt[] __attribute__((section(".text"))) = "\n> ";

/* Entry point: must be first in .text so bootloader jump to 0x7E00 hits it */
void kernel_main(void) __attribute__((section(".text.entry")));
void kernel_main(void)
{
	unsigned i;

	/* Clear screen */
	for (i = 0; i < VGA_SIZE; i++)
		VGA_BASE[i] = (unsigned short)(VGA_ATTR << 8);
	cursor = 0;
	cursor_cell = (unsigned short)(VGA_ATTR << 8);
	cursor_visible = 1;
	last_cursor_tick = 0;
	last_busy_tick = 0;
	busy_idx = 0;

	putstr(msg);
	putstr(prompt);

	timer_init();  /* PIT 1 kHz, IDT, sti; ticks counts ms */
	last_cursor_tick = ticks;
	last_busy_tick   = ticks;

	/* Drain keyboard buffer so we don't echo leftover/garbage bytes */
	while (inb(KB_STATUS) & 1)
		(void)inb(KB_DATA);

	/* Terminal echo loop: busy indicator, blinking cursor at cursor, then poll keyboard */
	for (;;) {
		update_busy();
		update_cursor_blink();
		if (inb(KB_STATUS) & 1) {
			unsigned char sc = inb(KB_DATA);
			if (sc & 0x80) {
				/* Set 1 break: release shift */
				if (sc == 0xAA || sc == 0xB6) shift_pressed = 0;
				continue;
			}
			if (sc == 0x2A || sc == 0x36) {
				shift_pressed = 1;
				continue;
			}
			{
				char c = (char)scancode_to_ascii[sc];
				if (c) {
					c = apply_shift(c);
					last_scancode = sc;
					debug_line(last_scancode, c);
					putchar(c);
					if (c == '\n')
						putstr(prompt);
				}
			}
		}
		else
			__asm__ volatile ("hlt");  /* wait for next interrupt (PIT or other) */
	}
}
