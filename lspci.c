/* lspci command: enumerate PCI functions.
 *
 *   lspci            one line per function:
 *                      bb:dd.f vvvv:dddd rNN class CCSS irq NN
 *   lspci <filter>   additionally print BAR details for every function whose
 *                    "vvvv:dddd" or "CCSS" contains <filter> (hex, case
 *                    insensitive), e.g. `lspci 8168` or `lspci 0200`.
 *
 * BAR sizes are probed by writing all-ones and restoring the original value
 * (the same trick lspci(8) uses).  Probing only happens for filter matches,
 * so a plain `lspci` never touches config space.
 */

#include "io.h"
#include "pci.h"

static void put_hex2(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
}

static void put_hex4(unsigned int v)
{
	put_hex2((v >> 8) & 0xFF);
	put_hex2(v & 0xFF);
}

static void put_hex8(unsigned int v)
{
	put_hex4((v >> 16) & 0xFFFF);
	put_hex4(v & 0xFFFF);
}

static void put_dec(unsigned int v)
{
	char b[12];
	int i = 0;

	if (v == 0) {
		io_putchar('0');
		return;
	}
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i)
		io_putchar(b[--i]);
}

static void put_size(unsigned int bytes)
{
	if (bytes == 0) {
		io_putchar('?');
	} else if (bytes >= 1024u * 1024u) {
		put_dec(bytes / (1024u * 1024u));
		io_putchar('M');
	} else if (bytes >= 1024u) {
		put_dec(bytes / 1024u);
		io_putchar('K');
	} else {
		put_dec(bytes);
		io_putchar('B');
	}
}

static void hex_str(char *dst, unsigned int v, unsigned int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned int i;

	for (i = 0; i < digits; i++)
		dst[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xF];
	dst[digits] = '\0';
}

static char lcc(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int contains_ci(const char *hay, const char *needle)
{
	unsigned int i, j;

	if (!needle[0])
		return 1;
	for (i = 0; hay[i]; i++) {
		for (j = 0; needle[j] && hay[i + j] &&
			    lcc(hay[i + j]) == lcc(needle[j]); j++)
			;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static const char *class_name(unsigned int cls)
{
	switch (cls) {
	case 0x00: return "legacy";
	case 0x01: return "storage";
	case 0x02: return "net";
	case 0x03: return "display";
	case 0x04: return "multimedia";
	case 0x06: return "bridge";
	case 0x08: return "system";
	case 0x0C: return "serial";
	default:   return 0;
	}
}

/* Print one BAR; returns the number of config slots consumed (2 for 64-bit). */
static unsigned int print_bar(unsigned int bus, unsigned int dev,
			      unsigned int func, unsigned int index)
{
	unsigned int reg = 0x10u + 4u * index;
	unsigned int val = pci_cfg_read32(bus, dev, func, reg);
	unsigned int is64, hi, mask;

	if (val == 0 || val == 0xFFFFFFFFu)
		return 1;                        /* not implemented */
	if (!(val & 1u) && !(val & 0xFFFFFFF0u))
		return 1;                        /* mem BAR without a base */

	io_putstr("  bar");
	put_dec(index);
	io_putstr((val & 1u) ? " i/o " : " mem ");

	if (val & 1u) {
		pci_cfg_write32(bus, dev, func, reg, 0xFFFFFFFFu);
		mask = pci_cfg_read32(bus, dev, func, reg) & 0xFFFFFFFCu;
		pci_cfg_write32(bus, dev, func, reg, val);
		put_hex4(val & 0xFFFCu);
		io_putstr(" (");
		put_size(mask ? (~mask + 1u) : 0);
		io_putchar(')');
		io_putchar('\n');
		return 1;
	}

	is64 = (val & 4u) ? 1u : 0u;
	hi = is64 ? pci_cfg_read32(bus, dev, func, reg + 4) : 0;
	pci_cfg_write32(bus, dev, func, reg, 0xFFFFFFFFu);
	if (is64)
		pci_cfg_write32(bus, dev, func, reg + 4, 0xFFFFFFFFu);
	mask = pci_cfg_read32(bus, dev, func, reg) & 0xFFFFFFF0u;
	pci_cfg_write32(bus, dev, func, reg, val);
	if (is64)
		pci_cfg_write32(bus, dev, func, reg + 4, hi);

	if (hi)
		put_hex8(hi);
	put_hex8(val & 0xFFFFFFF0u);
	io_putstr(" (");
	put_size(mask ? (~mask + 1u) : 0);
	if (is64)
		io_putstr(", 64b");
	if (val & 8u)
		io_putstr(", pf");
	io_putchar(')');
	io_putchar('\n');
	return is64 ? 2u : 1u;
}

void program_lspci_main(const char *filter)
{
	unsigned int bus, dev, func, nfunc, matches = 0;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int htype;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;

			/* Multifunction if header type bit 7 is set. */
			htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;

			for (func = 0; func < nfunc; func++) {
				unsigned int idf = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int class_rev, cls, sub, irq, b;
				const char *cn;
				char vd[10], cs[5];

				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;

				class_rev = pci_cfg_read32(bus, dev, func, 0x08);
				cls = (class_rev >> 24) & 0xFFu;
				sub = (class_rev >> 16) & 0xFFu;
				irq = pci_cfg_read32(bus, dev, func, 0x3C) & 0xFFu;

				hex_str(vd, idf & 0xFFFFu, 4);
				vd[4] = ':';
				hex_str(vd + 5, (idf >> 16) & 0xFFFFu, 4);
				hex_str(cs, (cls << 8) | sub, 4);

				put_hex2(bus);
				io_putchar(':');
				put_hex2(dev);
				io_putchar('.');
				io_putchar((char)('0' + func));
				io_putchar(' ');
				io_putstr(vd);
				io_putstr(" r");
				put_hex2(class_rev & 0xFFu);
				io_putstr(" class ");
				io_putstr(cs);
				cn = class_name(cls);
				if (cn) {
					io_putchar(' ');
					io_putstr(cn);
				}
				io_putstr(" irq ");
				put_dec(irq);
				io_putchar('\n');

				if (!contains_ci(vd, filter) && !contains_ci(cs, filter))
					continue;
				matches++;
				for (b = 0; b < 6; )
					b += print_bar(bus, dev, func, b);
			}
		}
	}

	if (filter[0] && matches == 0) {
		io_putstr("lspci: no match for ");
		io_putstr(filter);
		io_putchar('\n');
	}
}
