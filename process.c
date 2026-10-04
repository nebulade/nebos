/* Process management: create, exit, round-robin schedule, ps, kill. */

#include "process.h"
#include "idle.h"
#include "io.h"
#include "kmalloc.h"
#include "mm.h"
#include "serial.h"

struct process *procs;               /* dynamically-sized process table */
volatile unsigned int current_pid;

static unsigned int proc_cap;            /* current capacity of procs[] */
static unsigned int next_pid_counter = 1; /* monotonic, wraps */
static unsigned int fallback_stack_next;  /* 0..PROC_FALLBACK_STACK_MAX */

#define INITIAL_PROC_CAP 16

/* ---- interrupt flag save/restore (for short critical sections) ---- */
static inline unsigned int irq_save_disable(void)
{
	unsigned int flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

/* Allocate a process stack and return its BASE address (page-aligned), or 0.
 * *owned is set to 1 if it came from the frame allocator (must be freed on
 * exit), 0 if it came from the fallback pool. */
static unsigned int stack_alloc(int *owned)
{
	unsigned int base = mm_alloc_page();
	if (base) {
		*owned = 1;
		return base;
	}
	if (fallback_stack_next < PROC_FALLBACK_STACK_MAX) {
		unsigned int top = PROC_FALLBACK_STACK_BASE
			+ fallback_stack_next * PROC_STACK_SIZE
			+ PROC_STACK_SIZE;
		fallback_stack_next++;
		*owned = 0;
		return top - PROC_STACK_SIZE;
	}
	return 0;
}

static void name_copy(char *dst, const char *src)
{
	unsigned int i;
	for (i = 0; i < PROC_NAME_LEN - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* Double the process table.  Called with interrupts disabled. */
static int grow_proc_table(void)
{
	unsigned int new_cap = proc_cap * 2;
	struct process *np =
		(struct process *)kmalloc(new_cap * sizeof(struct process));
	unsigned int i;

	if (!np)
		return 0;
	for (i = 0; i < proc_cap; i++)
		np[i] = procs[i];
	for (i = proc_cap; i < new_cap; i++)
		np[i].state = PROC_UNUSED;
	kfree(procs);
	procs = np;
	proc_cap = new_cap;
	return 1;
}

/* Find a free process slot (1..proc_cap-1), growing the table if needed.
 * Slot 0 is the window manager and is never returned.  Called with interrupts
 * disabled.  Returns the slot index, or -1 on failure. */
static int alloc_slot(void)
{
	unsigned int slot;
	for (slot = 1; slot < proc_cap; slot++)
		if (procs[slot].state == PROC_UNUSED)
			return (int)slot;
	if (!grow_proc_table())
		return -1;
	for (slot = 1; slot < proc_cap; slot++)
		if (procs[slot].state == PROC_UNUSED)
			return (int)slot;
	return -1;
}

void process_init(void)
{
	unsigned int i;

	procs = (struct process *)kmalloc(INITIAL_PROC_CAP * sizeof(struct process));
	if (!procs) {
		serial_puts(SERIAL_COM1, "process_init: out of memory\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}
	proc_cap = INITIAL_PROC_CAP;
	for (i = 0; i < proc_cap; i++)
		procs[i].state = PROC_UNUSED;

	procs[0].state      = PROC_RUNNING;
	procs[0].pid        = 0;
	procs[0].esp        = 0;
	procs[0].screen     = (void *)0;
	procs[0].stack_base = 0;  /* bootstrap stack, never freed */
	name_copy(procs[0].name, "wm");
	current_pid = 0;
}

/*
 * Build initial stack frame for a new process so that popa + iret
 * "returns" to the entry point.  For arg-taking entries the argument
 * string is copied onto the stack and a pointer to it passed as the
 * first C parameter.
 *
 * Stack layout (high to low):
 *
 *   [copied arg string, if any, rounded up to 4-byte boundary]
 *   arg pointer           -- first C parameter (only if has_arg)
 *   process_exit address  -- return address seen by the entry function
 *   EFLAGS  (0x202)       -- IF=1
 *   CS      (0x08)
 *   EIP     (entry)       -- iret target
 *   EAX  ECX  EDX  EBX  ESP_orig  EBP  ESI  EDI   (pusha frame)
 *   <- saved ESP
 */
static int setup_process(const char *name, unsigned int entry,
			 int has_arg, const char *arg)
{
	int slot, owned;
	unsigned int base, top, *sp, arg_ptr = 0;
	unsigned int flags;

	flags = irq_save_disable();

	slot = alloc_slot();
	if (slot < 0) {
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		irq_restore(flags);
		return -1;
	}
	top = base + PROC_STACK_SIZE;
	sp = (unsigned int *)top;

	if (has_arg && arg) {
		unsigned int len, aligned, i;
		char *dst;
		for (len = 0; arg[len]; len++)
			;
		len++;
		aligned = (len + 3) & ~3u;
		sp = (unsigned int *)((unsigned int)sp - aligned);
		dst = (char *)sp;
		for (i = 0; i < len; i++)
			dst[i] = arg[i];
		for (; i < aligned; i++)
			dst[i] = '\0';
		arg_ptr = (unsigned int)sp;
	}

	if (has_arg)
		*--sp = arg_ptr;
	*--sp = (unsigned int)process_exit;
	*--sp = 0x202;          /* EFLAGS: IF=1 */
	*--sp = 0x08;           /* CS */
	*--sp = entry;          /* EIP */
	*--sp = 0;              /* EAX */
	*--sp = 0;              /* ECX */
	*--sp = 0;              /* EDX */
	*--sp = 0;              /* EBX */
	*--sp = 0;              /* ESP (ignored by popa) */
	*--sp = 0;              /* EBP */
	*--sp = 0;              /* ESI */
	*--sp = 0;              /* EDI */

	procs[slot].esp        = (unsigned int)sp;
	procs[slot].state      = PROC_READY;
	procs[slot].pid        = next_pid_counter++;
	procs[slot].stack_base = owned ? base : 0;
	name_copy(procs[slot].name, name);
	/* Child inherits the parent's output screen (command output lands in the
	   terminal that spawned it). */
	procs[slot].screen = procs[current_pid].screen;

	irq_restore(flags);
	return (int)procs[slot].pid;
}

int process_create(const char *name, void (*entry)(void))
{
	return setup_process(name, (unsigned int)entry, 0, (void *)0);
}

int process_create_arg(const char *name, void (*entry)(const char *),
		       const char *arg)
{
	return setup_process(name, (unsigned int)entry, 1, arg);
}

/* Create a process whose entry takes a raw pointer argument (no string copy). */
int process_create_ptr(const char *name, void (*entry)(void *), void *arg)
{
	int slot, owned;
	unsigned int base, top, *sp;
	unsigned int flags;

	flags = irq_save_disable();

	slot = alloc_slot();
	if (slot < 0) {
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		irq_restore(flags);
		return -1;
	}
	top = base + PROC_STACK_SIZE;
	sp = (unsigned int *)top;

	*--sp = (unsigned int)arg;        /* first C parameter */
	*--sp = (unsigned int)process_exit;/* return address */
	*--sp = 0x202;                    /* EFLAGS: IF=1 */
	*--sp = 0x08;                     /* CS */
	*--sp = (unsigned int)entry;      /* EIP */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* EAX ECX EDX EBX */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* ESP EBP ESI EDI */

	procs[slot].esp        = (unsigned int)sp;
	procs[slot].state      = PROC_READY;
	procs[slot].pid        = next_pid_counter++;
	procs[slot].stack_base = owned ? base : 0;
	name_copy(procs[slot].name, name);
	procs[slot].screen = procs[current_pid].screen;

	irq_restore(flags);
	return (int)procs[slot].pid;
}

/* Set the output screen of the currently-running process (used by a terminal
   at startup to bind itself to its own screen). */
void process_set_screen(struct text_screen *screen)
{
	procs[current_pid].screen = screen;
}

void process_exit(void)
{
	procs[current_pid].state = PROC_DEAD;
	for (;;)
		__asm__ volatile ("hlt");
}

/*
 * Round-robin scheduler.  Called from the PIT IRQ stub with interrupts
 * disabled.  Picks the next runnable process and updates current_pid.
 * The caller (switch.S) takes care of actually switching ESP.
 */
void schedule(void)
{
	unsigned int i, next;

	/* Reap dead processes, freeing their stacks.  Skip the current slot: we
	 * may be running on its stack right now (it just called process_exit),
	 * so it is reaped on a later schedule() from another process's context. */
	for (i = 0; i < proc_cap; i++) {
		if (procs[i].state == PROC_DEAD && i != current_pid) {
			if (procs[i].stack_base)
				mm_free_page(procs[i].stack_base);
			procs[i].stack_base = 0;
			procs[i].state = PROC_UNUSED;
		}
	}

	/* Mark current as READY (it was RUNNING). */
	if (procs[current_pid].state == PROC_RUNNING)
		procs[current_pid].state = PROC_READY;

	/* Find next runnable slot. */
	next = current_pid;
	for (i = 0; i < proc_cap; i++) {
		next = (next + 1) % proc_cap;
		if (procs[next].state == PROC_READY) {
			procs[next].state = PROC_RUNNING;
			current_pid = next;
			return;
		}
	}

	/* Nothing else runnable -- stay on current if still alive. */
	if (procs[current_pid].state == PROC_READY) {
		procs[current_pid].state = PROC_RUNNING;
		return;
	}

	/* Everything dead: fall back to slot 0 (window manager). */
	procs[0].state = PROC_RUNNING;
	current_pid = 0;
}

/* ---- user-facing helpers ---- */

static const char *state_str(unsigned int s)
{
	switch (s) {
	case PROC_RUNNING: return "run  ";
	case PROC_READY:   return "ready";
	default:           return "?    ";
	}
}

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

void process_ps(void)
{
	unsigned int i;
	io_putstr("PID  STATE  NAME\n");
	for (i = 0; i < proc_cap; i++) {
		if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD)
			continue;
		io_putchar(' ');
		put_dec(procs[i].pid);
		io_putstr("  ");
		io_putstr(state_str(procs[i].state));
		io_putstr("  ");
		io_putstr(procs[i].name);
		io_putchar('\n');
	}
}

int process_kill(unsigned int pid)
{
	unsigned int i;
	if (pid == 0)
		return -1; /* cannot kill window manager */
	for (i = 1; i < proc_cap; i++) {
		if (procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD &&
		    procs[i].pid == pid) {
			procs[i].state = PROC_DEAD;
			return 0;
		}
	}
	return -1;
}

/* Return 1 if a process with `pid` is still runnable (READY/RUNNING). */
int process_is_alive(unsigned int pid)
{
	unsigned int i;
	for (i = 0; i < proc_cap; i++)
		if (procs[i].pid == pid &&
		    (procs[i].state == PROC_READY ||
		     procs[i].state == PROC_RUNNING))
			return 1;
	return 0;
}

/* Wait (yield) until the process with `pid` has exited.  The scheduler
 * preempts us every timeslice so the child runs and process_exit() reaps
 * it to PROC_UNUSED. */
void process_wait(unsigned int pid)
{
	while (process_is_alive(pid))
		cpu_idle();
}
