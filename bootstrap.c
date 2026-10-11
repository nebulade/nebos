/* One-time kexec bootstrap (loaded via the existing `serial recv` + `run serial`
 * path at 9600 baud). Because nebos runs everything in ring-0, this program has
 * full hardware access: it switches COM1 to 115200, receives a new kernel image,
 * copies it over 0x7E00, and jumps to its kernel_main.
 *
 * Self-contained: no kernel symbols, no run_api dependency. We live at 0x43000
 * (outside the 0x7E00..0x42000 kernel region), so we can overwrite the kernel
 * directly from our own code without a relocated trampoline.
 *
 * Wire protocol (host -> us), after we emit 'R':
 *   4 bytes: magic "KXEC" (LE)
 *   4 bytes: entry address (LE)
 *   4 bytes: size (LE)
 *   N bytes: kernel.bin data
 *   1 byte:  checksum (sum of data bytes mod 256)
 */

#define UART_BASE        0x3F8
#define STAGE_ADDR       0x58000
#define KERNEL_LOAD_ADDR 0x7E00
#define BOOT_STACK       0x42800
#define MAGIC            0x4345584B /* "KXEC" little-endian */
#define MAX_SIZE         0x38200

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static void uart_115200(void)
{
	outb(UART_BASE + 1, 0x00); /* IER off */
	outb(UART_BASE + 3, 0x80); /* DLAB */
	outb(UART_BASE + 0, 0x01); /* divisor low = 1 (115200) */
	outb(UART_BASE + 1, 0x00); /* divisor high = 0 */
	outb(UART_BASE + 3, 0x03); /* 8N1 */
	outb(UART_BASE + 2, 0xC7); /* FIFO on, clear */
	outb(UART_BASE + 4, 0x03); /* DTR|RTS */
}

static unsigned char read_byte(void)
{
	while (!(inb(UART_BASE + 5) & 0x01))
		;
	return inb(UART_BASE + 0);
}

static unsigned int read32le(void)
{
	unsigned int v = 0;
	unsigned int i;
	for (i = 0; i < 4; i++)
		v |= (unsigned int)read_byte() << (8 * i);
	return v;
}

static void delay(void)
{
	volatile unsigned int i;
	for (i = 0; i < 50000; i++)
		;
}

static void halt(void)
{
	for (;;)
		__asm__ volatile ("hlt");
}

/* Flat GDT (base 0 / 4 GB) + GDTR, written to 0x900 (below the kernel) so
 * the new kernel's segment reloads don't read the old GDT we overwrite.
 * Layout: 24-byte GDT at 0x900, 6-byte GDTR at 0x918 (limit 23, base 0x900). */
static const unsigned char flat_gdt_blob[30] = {
	0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, /* null */
	0xFF,0xFF,0x00,0x00, 0x00,0x9A,0xCF,0x00, /* code */
	0xFF,0xFF,0x00,0x00, 0x00,0x92,0xCF,0x00, /* data */
	0x17,0x00,              /* gdtr limit = 23 */
	0x00,0x09,0x00,0x00     /* gdtr base = 0x900 */
};

static void copy_to(unsigned int dst, const void *src, unsigned int n)
{
	__asm__ volatile (
		"cld\n\t"
		"rep movsb"
		: "+S"(src), "+D"(dst), "+c"(n)
		:
		: "memory");
}

static void setup_flat_gdt(void)
{
	copy_to(0x900, flat_gdt_blob, sizeof(flat_gdt_blob));
	__asm__ volatile ("lgdt (%0)" : : "r"(0x918));
}

void _start(void)
{
	unsigned char c, sum = 0, chk;
	unsigned int magic, entry, size, i;

	/* Stop the PIT (and all IRQs) so the kernel's serial_poll stops
	 * competing with us for UART bytes. */
	__asm__ volatile ("cli");
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);

	uart_115200();

	/* Drain stale bytes. */
	while (inb(UART_BASE + 5) & 0x01)
		(void)inb(UART_BASE + 0);

	/* Emit 'R' until the host starts sending the header. */
	for (;;) {
		outb(UART_BASE + 0, 'R');
		delay();
		if (inb(UART_BASE + 5) & 0x01)
			break;
	}

	magic = read32le();
	entry = read32le();
	size  = read32le();

	if (magic != MAGIC || size == 0 || size > MAX_SIZE) {
		outb(UART_BASE + 0, 'E');
		halt();
	}

	for (i = 0; i < size; i++) {
		c = read_byte();
		((volatile unsigned char *)STAGE_ADDR)[i] = c;
		sum += c;
	}

	chk = read_byte();
	if (chk != sum) {
		outb(UART_BASE + 0, 'E');
		halt();
	}

	/* Copy staged kernel over 0x7E00, then jump to its kernel_main.
	 * We live at 0x43000, outside the overwritten region, so this is safe. */
	setup_flat_gdt();
	for (i = 0; i < size; i++)
		((volatile unsigned char *)KERNEL_LOAD_ADDR)[i] =
			((volatile unsigned char *)STAGE_ADDR)[i];

	__asm__ volatile ("movl %0, %%esp; jmp *%1" : : "i"(BOOT_STACK), "r"(entry));
}
