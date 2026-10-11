/* peek command: hex dump of memory (identity-mapped, so this also covers
 * MMIO registers, e.g. a driver's BAR once mapped).
 *
 *   peek <addr> [len]      default len 64, max 256
 *   poke <addr> <val>      write one dword (e.g. an MMIO register), print
 *                          the read-back
 *
 * Reading an unmapped address faults; the exception handler shows EIP/CR2.
 */

#include "io.h"

static void put_hex2(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
}

static void put_hex8(unsigned int v)
{
	put_hex2((v >> 24) & 0xFF);
	put_hex2((v >> 16) & 0xFF);
	put_hex2((v >> 8) & 0xFF);
	put_hex2(v & 0xFF);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex(const char *s, unsigned int *out, const char **endp)
{
	unsigned int v = 0;
	int digits = 0, d;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while ((d = hexval(*s)) >= 0) {
		v = (v << 4) | (unsigned int)d;
		digits++;
		s++;
	}
	if (!digits)
		return -1;
	*out = v;
	*endp = s;
	return 0;
}

void program_peek_main(const char *arg)
{
	const char *p = arg;
	const volatile unsigned char *mem;
	unsigned int addr, len = 64, i, j;

	if (parse_hex(p, &addr, &p) != 0) {
		io_putstr("peek: usage peek <hexaddr> [len<=256]\n");
		return;
	}
	while (*p == ' ')
		p++;
	if (*p) {
		if (parse_hex(p, &len, &p) != 0 || len == 0 || len > 256) {
			io_putstr("peek: usage peek <hexaddr> [len<=256]\n");
			return;
		}
	}
	while (*p == ' ')
		p++;
	if (*p) {
		io_putstr("peek: usage peek <hexaddr> [len<=256]\n");
		return;
	}

	mem = (const volatile unsigned char *)addr;
	for (i = 0; i < len; i += 16) {
		put_hex8(addr + i);
		io_putstr(": ");
		for (j = 0; j < 16 && i + j < len; j++) {
			put_hex2(mem[i + j]);
			io_putchar(' ');
		}
		io_putchar('\n');
	}
}

/* poke <addr> <val>: write one 32-bit dword and print the read-back.  Used to
 * drive MMIO experiments (e.g. the GPU's PIO method interface) from the
 * shell. */
void program_poke_main(const char *arg)
{
	const char *p = arg;
	volatile unsigned int *mem;
	unsigned int addr, val;

	if (parse_hex(p, &addr, &p) != 0) {
		io_putstr("poke: usage poke <hexaddr> <hexval>\n");
		return;
	}
	while (*p == ' ')
		p++;
	if (parse_hex(p, &val, &p) != 0) {
		io_putstr("poke: usage poke <hexaddr> <hexval>\n");
		return;
	}
	while (*p == ' ')
		p++;
	if (*p) {
		io_putstr("poke: usage poke <hexaddr> <hexval>\n");
		return;
	}

	mem = (volatile unsigned int *)(addr & ~3u);
	*mem = val;

	io_putstr("poke ");
	put_hex8(addr & ~3u);
	io_putstr(" <- ");
	put_hex8(val);
	io_putstr(" reads ");
	put_hex8(*mem);
	io_putchar('\n');
}
