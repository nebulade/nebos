/* top: ring-3 command.  Live process list with recent CPU and memory hints.
 *
 * The kernel credits every PIT tick to the process that was running it
 * (SYS_PROCINFO exposes the counters), so each frame samples the counters
 * twice, one second apart: CPU% is the share of ticks in that window.  The
 * WM slot ("wm") runs the idle loop, so on an idle system it owns nearly
 * every tick -- the footer says as much.
 *
 * The frame is repainted in place.  It has a fixed shape (23 rows x 63
 * columns + newlines), so the number of characters written per frame is a
 * constant; after the first paint the anchor is recomputed as
 * (cursor - FRAME_CHARS), which also corrects a scroll when the first frame
 * starts near the bottom of the screen.  Every row is padded to 63 columns,
 * so stale text from a previous frame is erased, and the last row is written
 * without a trailing newline so a frame that ends on the bottom row cannot
 * scroll.  Ctrl+C quits: the terminal kills its foreground command.
 */

#include "ucmd_lib.h"

#define FRAME_COLS  63            /* terminal screens are 64x32 (kernel screen.h) */
#define COL_PID     4
#define COL_STATE   7
#define COL_CPU     6
#define COL_TIME    6
#define COL_MEM     6
/* five single-space separators between the six columns */
#define NAME_W      (FRAME_COLS - COL_PID - COL_STATE - COL_CPU - COL_TIME - COL_MEM - 5)
#define PRINT_ROWS  20            /* process rows (header + footer excluded) */
#define MAX_SAMPLE  32            /* processes captured per sample */
#define FRAME_ROWS  (3 + PRINT_ROWS)
#define FRAME_CHARS (FRAME_ROWS * 64 - 1)  /* 63 chars + '\n' per row, no final '\n' */

/* CPUs ticks before the sampling window, matched by pid. */
struct sample {
	unsigned int pid;
	unsigned int cpu_ticks;
};

static struct sample before[MAX_SAMPLE];
static unsigned int nbefore;

struct prow {
	unsigned int pid;
	unsigned int state;
	unsigned int cpu_pm;     /* CPU per mille over the window */
	unsigned int cpu_ticks;  /* total ticks spent running */
	unsigned int pages;      /* resident pages */
	char name[12];
};

static struct prow rows[PRINT_ROWS];
static unsigned int nrows, ntotal;
static unsigned int wall;                 /* sample window, ms */
static unsigned int uptime_s;
static unsigned int free_kb, total_kb;
static unsigned int line_chars;           /* chars written into the current row */

static void putc_(char c)
{
	u_putc(c);
	line_chars++;
}

static void puts_(const char *s)
{
	while (*s)
		putc_(*s++);
}

static void dec_(unsigned int v)
{
	char buf[10];
	unsigned int n = 0;

	if (v == 0)
		buf[n++] = '0';
	while (v) {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (n)
		putc_(buf[--n]);
}

static void pad_(unsigned int n)
{
	while (n--)
		putc_(' ');
}

/* Right-aligned decimal in `width` columns. */
static void dec_w(unsigned int v, unsigned int width)
{
	unsigned int digits = 1, t = v;

	while (t >= 10) {
		t /= 10;
		digits++;
	}
	if (digits < width)
		pad_(width - digits);
	dec_(v);
}

/* Left-aligned string, truncated/padded to exactly `width` columns. */
static void str_w(const char *s, unsigned int width)
{
	unsigned int n = 0;

	while (s[n] && n < width)
		putc_(s[n++]);
	if (n < width)
		pad_(width - n);
}

/* Right-aligned string (for the column headers, to sit over right-aligned
 * numbers). */
static void str_wr(const char *s, unsigned int width)
{
	unsigned int n = 0, i;

	while (s[n])
		n++;
	if (n < width)
		pad_(width - n);
	for (i = 0; i < n; i++)
		putc_(s[i]);
}

/* Right-aligned "x.y%" in COL_CPU columns. */
static void cpu_w(unsigned int per_mille)
{
	unsigned int whole = per_mille / 10;
	unsigned int tenth = per_mille % 10;
	unsigned int digits = 1, t = whole;

	while (t >= 10) {
		t /= 10;
		digits++;
	}
	pad_(COL_CPU - (digits + 3));
	dec_(whole);
	putc_('.');
	putc_((char)('0' + tenth));
	putc_('%');
}

static const char *state_name(unsigned int s)
{
	if (s == 2)
		return "run";
	if (s == 1)
		return "ready";
	if (s == 4)
		return "sleep";
	return "?";
}

/* Pad the current row to full width; rows other than the last end with a
 * newline, which combined with the padding keeps every frame the same size. */
static void end_line(int last)
{
	while (line_chars < FRAME_COLS)
		putc_(' ');
	if (!last) {
		u_putc('\n');
		line_chars = 0;
	}
}

/* Take the "after" sample and turn it into rows, CPU% = ticks during the
 * window (a process that appeared during the window shows 0%). */
static void gather(void)
{
	struct sys_procinfo p;
	unsigned int i, j;

	ntotal = 0;
	nrows = 0;
	for (i = 0; ; i++) {
		if (u_syscall2(SYS_PROCINFO, (int)i, (int)&p) != 1)
			break;
		if (ntotal < PRINT_ROWS) {
			struct prow *r = &rows[ntotal];
			unsigned int delta = 0, pm = 0;

			for (j = 0; j < nbefore; j++)
				if (before[j].pid == p.pid) {
					delta = p.cpu_ticks - before[j].cpu_ticks;
					break;
				}
			if (j < nbefore) {
				pm = delta * 1000u / wall;
				if (pm > 1000u)
					pm = 1000u;
			}
			r->pid = p.pid;
			r->state = p.state;
			r->cpu_pm = pm;
			r->cpu_ticks = p.cpu_ticks;
			r->pages = p.pages;
			for (j = 0; j < sizeof(r->name); j++)
				r->name[j] = p.name[j];
			nrows++;
		}
		ntotal++;
	}
}

static void paint(void)
{
	unsigned int i;

	line_chars = 0;

	/* status line */
	puts_("top: up ");
	dec_(uptime_s);
	puts_("s  ");
	dec_(ntotal);
	puts_(ntotal == 1 ? " proc  " : " procs  ");
	puts_("mem ");
	dec_(free_kb);
	puts_("K free / ");
	dec_(total_kb);
	puts_("K");
	end_line(0);

	/* column headers */
	str_wr("PID", COL_PID);
	putc_(' ');
	str_w("STATE", COL_STATE);
	putc_(' ');
	str_wr("CPU%", COL_CPU);
	putc_(' ');
	str_wr("TIME", COL_TIME);
	putc_(' ');
	str_wr("MEM", COL_MEM);
	putc_(' ');
	str_w("NAME", NAME_W);
	end_line(0);

	for (i = 0; i < PRINT_ROWS; i++) {
		if (i < nrows) {
			const struct prow *r = &rows[i];

			dec_w(r->pid, COL_PID);
			putc_(' ');
			str_w(state_name(r->state), COL_STATE);
			putc_(' ');
			cpu_w(r->cpu_pm);
			putc_(' ');
			dec_w(r->cpu_ticks / 1000, COL_TIME - 1);
			putc_('s');
			putc_(' ');
			dec_w(r->pages * 4, COL_MEM - 1);
			putc_('K');
			putc_(' ');
			str_w(r->name, NAME_W);
		}
		end_line(0);
	}

	if (ntotal > nrows) {
		putc_('+');
		dec_(ntotal - nrows);
		puts_(" more  ");
	}
	puts_("Ctrl+C to quit - wm = idle loop");
	end_line(1);
}

static void frame(void)
{
	gather();
	paint();
}

void _start(const char *arg)
{
	struct sys_meminfo m;
	struct sys_procinfo p;
	unsigned int i, t0, t1;

	(void)arg;

	u_syscall1(SYS_MEMINFO, (int)&m);
	total_kb = m.total_kb;
	free_kb = m.free_pages * (m.page_size / 1024);

	for (;;) {
		/* Sample before the wait, repaint after it. */
		nbefore = 0;
		for (i = 0; i < MAX_SAMPLE; i++) {
			if (u_syscall2(SYS_PROCINFO, (int)i, (int)&p) != 1)
				break;
			before[nbefore].pid = p.pid;
			before[nbefore].cpu_ticks = p.cpu_ticks;
			nbefore++;
		}
		t0 = u_ticks_ms();
		u_sleep_ms(1000);
		t1 = u_ticks_ms();
		wall = t1 - t0;
		if (!wall)
			wall = 1;
		uptime_s = t1 / 1000;

		u_puts("\x1b[2J\x1b[H");
		frame();
	}
}
