/* TTY output rings.  Commands write bytes; the terminal process drains and
 * renders them.  A full ring blocks the writer (a terminal that is not
 * scheduled will drain shortly), so `cat`/helpers cannot silently lose output. */

#include "tty.h"
#include "mm.h"
#include "process.h"

#define TTY_STRIDE (2 * PAGE_SIZE)

struct tty {
	int used;
	unsigned int owner;
	unsigned char buf[TTY_BUF];
	unsigned int head, tail;
};

_Static_assert(sizeof(struct tty) <= TTY_STRIDE,
	       "tty state no longer fits its two-page slot");

/* The rings are 16 KB of state; keep them out of BSS so the kernel image
 * stays below the run area. */
static struct tty *ttys;
static int tty_ready;

static int tty_init(void)
{
	unsigned int base, i;
	unsigned char *p;

	if (tty_ready)
		return 1;
	base = mm_alloc_pages(TTY_MAX * (TTY_STRIDE / PAGE_SIZE));
	if (!base)
		return 0;
	p = (unsigned char *)base;
	for (i = 0; i < TTY_MAX * TTY_STRIDE; i++)
		p[i] = 0;
	ttys = (struct tty *)base;
	tty_ready = 1;
	return 1;
}

static struct tty *tty_slot(unsigned int i)
{
	return (struct tty *)((unsigned char *)ttys + i * TTY_STRIDE);
}

static struct tty *tty_get(int id)
{
	if (!ttys || id <= 0 || id > TTY_MAX)
		return 0;
	return tty_slot((unsigned int)(id - 1));
}

int tty_valid(int id)
{
	struct tty *t = tty_get(id);

	return t && t->used;
}

int tty_create(void)
{
	unsigned int i;

	if (!tty_init())
		return -1;
	for (i = 0; i < TTY_MAX; i++) {
		struct tty *t = tty_slot(i);

		if (!t->used) {
			t->used = 1;
			t->owner = procs[current_pid].pid;
			t->head = t->tail = 0;
			procs[current_pid].tty = (int)(i + 1);
			return (int)(i + 1);
		}
	}
	return -1;
}

void tty_close(int id)
{
	struct tty *t = tty_get(id);

	if (t) {
		t->used = 0;
		t->head = t->tail = 0;
	}
}

int tty_write(int id, const char *buf, unsigned int len)
{
	struct tty *t;
	unsigned int i = 0;

	if (!(t = tty_get(id)) || !t->used)
		return -1;
	while (i < len) {
		unsigned int next = (t->head + 1) % TTY_BUF;

		if (next == t->tail) {
			if (!process_is_alive(t->owner))
				return -1;
			process_sleep_ms(1);
			continue;
		}
		t->buf[t->head] = (unsigned char)buf[i++];
		t->head = next;
	}
	return 0;
}

int tty_write_str(int id, const char *s)
{
	unsigned int n = 0;

	while (s[n])
		n++;
	return tty_write(id, s, n);
}

int tty_read(int id, char *buf, unsigned int max)
{
	struct tty *t;
	unsigned int n = 0;

	if (!(t = tty_get(id)) || !t->used)
		return -1;
	while (n < max && t->tail != t->head) {
		buf[n++] = (char)t->buf[t->tail];
		t->tail = (t->tail + 1) % TTY_BUF;
	}
	return (int)n;
}

void tty_reap(unsigned int pid)
{
	unsigned int i;

	if (!ttys)
		return;
	for (i = 0; i < TTY_MAX; i++) {
		struct tty *t = tty_slot(i);

		if (t->used && t->owner == pid)
			t->used = 0;
	}
}
