/* lspci command: enumerate PCI devices (vendor:device, class, BAR0). */

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

void program_lspci_main(void)
{
	unsigned int bus, dev;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int class_rev, bar0;
			unsigned int vendor, device, cls, sub;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;

			vendor = id & 0xFFFFu;
			device = (id >> 16) & 0xFFFFu;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			cls = (class_rev >> 24) & 0xFFu;
			sub = (class_rev >> 16) & 0xFFu;
			bar0 = pci_cfg_read32(bus, dev, 0, 0x10);

			put_hex2(bus);
			io_putchar(':');
			put_hex2(dev);
			io_putstr("  ");
			put_hex4(vendor);
			io_putchar(':');
			put_hex4(device);
			io_putstr("  class=");
			put_hex2(cls);
			put_hex2(sub);
			io_putstr("  bar0=");
			put_hex8(bar0);
			io_putchar('\n');
		}
	}
}
