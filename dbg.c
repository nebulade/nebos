/* Serial debug monitor. Entered via the `dbg` command and driven from the host
 * with `make monitor` (picocom). ASCII commands, newline-terminated, hex args:
 *
 *   r <addr> <len>           read len bytes at addr -> hex dump
 *   w <addr> <b0> [b1 ...]   write bytes at addr
 *   i <port>                 inb (read I/O port)
 *   o <port> <val>           outb (write I/O port)
 *   h                        help
 *   q                        quit
 *
 * Runs inline (blocks the terminal), so it owns the serial ring buffer and
 * doesn't conflict with the kexec auto-detect in the terminal idle loop.
 */

#include "dbg.h"
#include "serial.h"

#define DBG_LINE_MAX 64
#define DBG_NARGS    32
#define DBG_READ_MAX 256

static inline unsigned char dbg_inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void dbg_outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void dbg_putbyte(unsigned char b)
{
	static const char hex[] = "0123456789ABCDEF";
	serial_putc(SERIAL_COM1, hex[b >> 4]);
	serial_putc(SERIAL_COM1, hex[b & 0xF]);
}

/* Block until a byte arrives (echoing it so picocom works without local echo). */
static void dbg_getc(unsigned char *c)
{
	for (;;) {
		if (!serial_ring_wait(0xFFFFFFFFu, c))
			continue;              /* timeout (effectively never) */
		if (*c == '\r')
			;                      /* ignore bare CR */
		else if (*c == '\n')
			serial_puts(SERIAL_COM1, "\n");
		else
			serial_putc(SERIAL_COM1, (char)*c);
		return;
	}
}

static void dbg_read_line(char *buf, unsigned int max)
{
	unsigned int i = 0;
	unsigned char c;
	for (;;) {
		dbg_getc(&c);
		if (c == '\n' || c == '\r')
			break;
		if (i + 1 < max)
			buf[i++] = (char)c;
	}
	buf[i] = '\0';
}

/* Parse a hex number at *s (optional 0x prefix); advance *s past it. */
static int dbg_parse_hex(const char **s, unsigned int *out)
{
	const char *p = *s;
	unsigned int v = 0;
	int digits = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	for (;; p++) {
		unsigned int d;
		char c = *p;
		if (c >= '0' && c <= '9')       d = (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')  d = (unsigned int)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')  d = (unsigned int)(c - 'A' + 10);
		else break;
		v = (v << 4) | d;
		digits++;
	}
	if (!digits)
		return 0;
	*out = v;
	*s = p;
	return 1;
}

/* Returns 1 to quit. */
static int dbg_command(const char *line)
{
	const char *p = line;
	unsigned int args[DBG_NARGS];
	unsigned int nargs = 0;
	char cmd;

	while (*p == ' ')
		p++;
	if (!*p)
		return 0;
	cmd = *p++;

	while (nargs < DBG_NARGS) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (!dbg_parse_hex(&p, &args[nargs]))
			break;
		nargs++;
	}

	switch (cmd) {
	case 'r':
		if (nargs < 2) {
			serial_puts(SERIAL_COM1, "usage: r <addr> <len>\n");
		} else {
			unsigned int addr = args[0];
			unsigned int len = args[1];
			unsigned int i;
			if (len > DBG_READ_MAX)
				len = DBG_READ_MAX;
			serial_puthex(SERIAL_COM1, addr);
			serial_putc(SERIAL_COM1, ':');
			for (i = 0; i < len; i++) {
				serial_putc(SERIAL_COM1, ' ');
				dbg_putbyte(((volatile unsigned char *)addr)[i]);
			}
			serial_putc(SERIAL_COM1, '\n');
		}
		return 0;

	case 'w':
		if (nargs < 2) {
			serial_puts(SERIAL_COM1, "usage: w <addr> <b0> [b1 ...]\n");
		} else {
			unsigned int addr = args[0];
			unsigned int i;
			for (i = 1; i < nargs; i++)
				((volatile unsigned char *)addr)[i - 1] = (unsigned char)args[i];
			serial_puts(SERIAL_COM1, "ok\n");
		}
		return 0;

	case 'i':
		if (nargs < 1) {
			serial_puts(SERIAL_COM1, "usage: i <port>\n");
		} else {
			serial_puthex(SERIAL_COM1, args[0]);
			serial_puts(SERIAL_COM1, " = ");
			dbg_putbyte(dbg_inb((unsigned short)args[0]));
			serial_putc(SERIAL_COM1, '\n');
		}
		return 0;

	case 'o':
		if (nargs < 2) {
			serial_puts(SERIAL_COM1, "usage: o <port> <val>\n");
		} else {
			dbg_outb((unsigned short)args[0], (unsigned char)args[1]);
			serial_puts(SERIAL_COM1, "ok\n");
		}
		return 0;

	case 'h':
		serial_puts(SERIAL_COM1,
			"r <addr> <len>   read memory (hex dump)\n"
			"w <addr> <b>...  write bytes\n"
			"i <port>         inb\n"
			"o <port> <val>   outb\n"
			"h                help\n"
			"q                quit\n");
		return 0;

	case 'q':
		return 1;

	default:
		serial_puts(SERIAL_COM1, "unknown cmd (h for help)\n");
		return 0;
	}
}

void program_dbg_main(void)
{
	char line[DBG_LINE_MAX];

	serial_puts(SERIAL_COM1, "\ndbg: ready (h for help)\n");
	for (;;) {
		serial_puts(SERIAL_COM1, "dbg> ");
		dbg_read_line(line, DBG_LINE_MAX);
		if (dbg_command(line))
			break;
	}
	serial_puts(SERIAL_COM1, "dbg: bye\n");
}
