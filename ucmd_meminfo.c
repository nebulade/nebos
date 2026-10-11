/* meminfo: ring-3 command.  Total/free physical memory and the E820 map. */

#include "ucmd_lib.h"

void _start(const char *arg)
{
	struct sys_meminfo m;
	unsigned int i;

	(void)arg;
	u_syscall1(SYS_MEMINFO, (int)&m);

	u_puts("memory: ");
	u_putdec(m.total_kb);
	u_puts(" KB total, ");
	u_putdec(m.free_pages * (m.page_size / 1024));
	u_puts(" KB free (");
	u_putdec(m.free_pages);
	u_puts(" pages)\n");

	if (m.e820_count == 0) {
		u_puts("E820 map unavailable\n");
		u_exit(0);
	}

	u_puts("E820 map (");
	u_putdec(m.e820_count);
	u_puts(" entries):\n");
	for (i = 0; i < m.e820_count; i++) {
		struct sys_e820 e;

		if (u_syscall2(SYS_E820, (int)i, (int)&e) != 1)
			break;
		u_puts("  [0x");
		u_puthex(e.base);
		u_puts(" +0x");
		u_puthex(e.length);
		u_puts("] type ");
		u_putdec(e.type);
		u_putc('\n');
	}
	u_exit(0);
}
