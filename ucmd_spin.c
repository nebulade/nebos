/* spin: CPU-bound test process for scheduler/priority checks.
 *
 * Usage: spin [ms]   (default 5000 ms)
 */
#include "ucmd_lib.h"

static unsigned int parse_ms(const char *s)
{
	unsigned int n = 0;

	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (unsigned int)(*s - '0');
		s++;
	}
	return n;
}

void _start(const char *arg)
{
	unsigned int ms = parse_ms(arg);
	unsigned int t0, i;
	unsigned long loops = 0;

	if (ms == 0)
		ms = 5000;
	t0 = u_ticks_ms();
	while (u_ticks_ms() - t0 < ms) {
		for (i = 0; i < 10000; i++)
			__asm__ volatile ("" ::: "memory");
		loops++;
	}
	u_puts("spin: done (");
	u_putdec(loops);
	u_puts(" loops)\n");
	u_exit(0);
}
