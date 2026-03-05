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

/* PS/2 keyboard: data port 0x60, status 0x64 (bit 0 = output buffer full) */
#define KB_DATA  0x60
#define KB_STATUS 0x64

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

static unsigned int cursor;  /* linear position 0 .. CONTENT_SIZE-1 */
static unsigned char last_scancode;     /* set by getchar for debug line */
#define BUSY_POS (DEBUG_ROW * VGA_COLS + 79)  /* bottom-right: never overwritten by echo */
static unsigned int busy_tick;
/* Explicit ASCII so no encoding/escape issues: - \ | / */
static const unsigned char busy_chars[4] = { 0x2D, 0x5C, 0x7C, 0x2F };

static void putchar(char c)
{
	if (c == '\n') {
		cursor = (cursor / VGA_COLS + 1) * VGA_COLS;
		if (cursor >= CONTENT_SIZE) {
			/* Scroll: move lines up, clear bottom of content area */
			unsigned int i;
			for (i = 0; i < (CONTENT_ROWS - 1) * VGA_COLS; i++)
				VGA_BASE[i] = VGA_BASE[i + VGA_COLS];
			for (i = (CONTENT_ROWS - 1) * VGA_COLS; i < CONTENT_SIZE; i++)
				VGA_BASE[i] = (unsigned short)(VGA_ATTR << 8);
			cursor = (CONTENT_ROWS - 1) * VGA_COLS;
		}
		return;
	}
	if (c == '\b') {
		if (cursor > 0) {
			cursor--;
			VGA_BASE[cursor] = (unsigned short)(VGA_ATTR << 8);
		}
		return;
	}
	if (cursor < CONTENT_SIZE) {
		VGA_BASE[cursor] = (unsigned short)((VGA_ATTR << 8) | (unsigned char)c);
		cursor++;
	}
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

/* Draw busy indicator at bottom-right so we know the event loop runs. */
static void update_busy(void)
{
	busy_tick++;
	VGA_BASE[BUSY_POS] = (unsigned short)((VGA_ATTR << 8) | busy_chars[busy_tick % 4]);
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

	putstr(msg);
	putstr(prompt);

	/* Drain keyboard buffer so we don't echo leftover/garbage bytes */
	while (inb(KB_STATUS) & 1)
		(void)inb(KB_DATA);

	/* Terminal echo loop: always update busy indicator, then poll keyboard */
	for (;;) {
		update_busy();
		if (inb(KB_STATUS) & 1) {
			unsigned char sc = inb(KB_DATA);
			if (sc & 0x80)
				continue;  /* Set 1 break code: ignore */
			{
				char c = (char)scancode_to_ascii[sc];
				if (c) {
					last_scancode = sc;
					debug_line(last_scancode, c);
					putchar(c);
					if (c == '\n')
						putstr(prompt);
				}
			}
		}
	}
}
