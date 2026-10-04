/* Process management: create, exit, round-robin schedule, ps, kill. */

#include "process.h"
#include "idle.h"
#include "io.h"

struct process procs[MAX_PROCS];
volatile unsigned int current_pid;

static unsigned int next_pid_counter = 1; /* monotonic, wraps */

/* Stack top for process slot (1..MAX_PROCS-1).  Slot 0 uses bootstrap stack. */
static unsigned int stack_top_for(unsigned int slot)
{
	return PROC_STACK_BASE + slot * PROC_STACK_SIZE;
}

static void name_copy(char *dst, const char *src)
{
	unsigned int i;
	for (i = 0; i < PROC_NAME_LEN - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

void process_init(void)
{
	unsigned int i;
	for (i = 0; i < MAX_PROCS; i++)
		procs[i].state = PROC_UNUSED;

	procs[0].state  = PROC_RUNNING;
	procs[0].pid    = 0;
	procs[0].screen = (void *)0;
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
	unsigned int slot, top, *sp, arg_ptr = 0;

	for (slot = 1; slot < MAX_PROCS; slot++)
		if (procs[slot].state == PROC_UNUSED)
			break;
	if (slot == MAX_PROCS)
		return -1;

	top = stack_top_for(slot);

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

	procs[slot].esp    = (unsigned int)sp;
	procs[slot].state  = PROC_READY;
	procs[slot].pid    = next_pid_counter++;
	name_copy(procs[slot].name, name);
	/* Child inherits the parent's output screen (command output lands in the
	   terminal that spawned it). */
	procs[slot].screen = procs[current_pid].screen;

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
	unsigned int slot, top, *sp;

	for (slot = 1; slot < MAX_PROCS; slot++)
		if (procs[slot].state == PROC_UNUSED)
			break;
	if (slot == MAX_PROCS)
		return -1;

	top = stack_top_for(slot);
	sp = (unsigned int *)top;

	*--sp = (unsigned int)arg;        /* first C parameter */
	*--sp = (unsigned int)process_exit;/* return address */
	*--sp = 0x202;                    /* EFLAGS: IF=1 */
	*--sp = 0x08;                     /* CS */
	*--sp = (unsigned int)entry;      /* EIP */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* EAX ECX EDX EBX */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* ESP EBP ESI EDI */

	procs[slot].esp    = (unsigned int)sp;
	procs[slot].state  = PROC_READY;
	procs[slot].pid    = next_pid_counter++;
	name_copy(procs[slot].name, name);
	procs[slot].screen = procs[current_pid].screen;

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

	/* Clean up dead processes. */
	for (i = 0; i < MAX_PROCS; i++)
		if (procs[i].state == PROC_DEAD)
			procs[i].state = PROC_UNUSED;

	/* Mark current as READY (it was RUNNING). */
	if (procs[current_pid].state == PROC_RUNNING)
		procs[current_pid].state = PROC_READY;

	/* Find next runnable slot. */
	next = current_pid;
	for (i = 0; i < MAX_PROCS; i++) {
		next = (next + 1) % MAX_PROCS;
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

	/* Everything dead: fall back to slot 0 (terminal). */
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

void process_ps(void)
{
	unsigned int i;
	io_putstr("PID  STATE  NAME\n");
	for (i = 0; i < MAX_PROCS; i++) {
		if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD)
			continue;
		io_putchar(' ');
		if (procs[i].pid < 10)
			io_putchar(' ');
		if (procs[i].pid >= 10)
			io_putchar((char)('0' + procs[i].pid / 10));
		io_putchar((char)('0' + procs[i].pid % 10));
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
		return -1; /* cannot kill terminal */
	for (i = 1; i < MAX_PROCS; i++) {
		if (procs[i].state != PROC_UNUSED && procs[i].state != PROC_DEAD &&
		    procs[i].pid == pid) {
			procs[i].state = PROC_DEAD;
			return 0;
		}
	}
	return -1;
}

/* Wait (yield) until the process with `pid` has exited.  The scheduler
 * preempts us every timeslice so the child runs and process_exit() reaps
 * it to PROC_UNUSED. */
void process_wait(unsigned int pid)
{
	unsigned int i;
	for (;;) {
		int alive = 0;
		for (i = 0; i < MAX_PROCS; i++)
			if (procs[i].pid == pid &&
			    (procs[i].state == PROC_READY ||
			     procs[i].state == PROC_RUNNING))
				alive = 1;
		if (!alive)
			return;
		cpu_idle();
	}
}
