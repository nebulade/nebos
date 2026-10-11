/* meminfo command: report total/free physical memory and the E820 map. */

#include "io.h"
#include "mm.h"

static void put_dec(unsigned int n)
{
	char buf[12];
	unsigned int i = sizeof(buf) - 1;
	buf[i] = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i > 0) {
		buf[--i] = (char)('0' + (n % 10));
		n /= 10;
	}
	io_putstr(buf + i);
}

void program_mem_main(void)
{
	unsigned int total_kb = mm_total_ram_kb();
	unsigned int free_pages = mm_free_pages();
	unsigned int free_kb = free_pages * (PAGE_SIZE / 1024);
	unsigned int n = mm_e820_count();
	unsigned int i;

	io_putstr("memory: ");
	put_dec(total_kb);
	io_putstr(" KB total, ");
	put_dec(free_kb);
	io_putstr(" KB free (");
	put_dec(free_pages);
	io_putstr(" pages)\n");

	if (n == 0) {
		io_putstr("E820 map unavailable\n");
		return;
	}

	io_putstr("E820 map (");
	put_dec(n);
	io_putstr(" entries):\n");
	for (i = 0; i < n; i++) {
		const struct e820_entry *e = mm_e820_entry(i);
		io_putstr("  [0x");
		io_puthex((unsigned int)e->base);
		io_putstr(" +0x");
		io_puthex((unsigned int)e->length);
		io_putstr("] type ");
		put_dec(e->type);
		io_putchar('\n');
	}
}
