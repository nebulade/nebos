/* hello: a real ring-3 user process.
 *
 * Prints "Hello user this is a real process!" once per second (10 times),
 * then "Goodbye", and exits — all through int 0x80 syscalls. */
#define SYS_PUTSTR       1
#define SYS_GET_TICKS_MS 2
#define SYS_SLEEP_MS     3
#define SYS_EXIT         4

static inline int syscall1(int num, int a1)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num), "b"(a1)
			  : "memory");
	return ret;
}

static inline int syscall0(int num)
{
	int ret;
	__asm__ volatile ("int $0x80"
			  : "=a"(ret)
			  : "a"(num)
			  : "memory");
	return ret;
}

static void putstr(const char *s) { syscall1(SYS_PUTSTR, (int)s); }
static void sleep_ms(unsigned int ms) { syscall1(SYS_SLEEP_MS, (int)ms); }

void _start(void)
{
	int i;
	for (i = 0; i < 10; i++) {
		sleep_ms(1000);
		putstr("Hello user this is a real process!\n");
	}
	putstr("Goodbye\n");
	syscall1(SYS_EXIT, 0);
	for (;;)
		;
}
