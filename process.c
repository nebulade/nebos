/* Process management: create, exit, round-robin schedule, ps, kill. */

#include "process.h"
#include "elf.h"
#include "idle.h"
#include "io.h"
#include "kmalloc.h"
#include "mm.h"
#include "paging.h"
#include "serial.h"
#include "sys_fs.h"
#include "sys_net.h"
#include "syscall.h"
#include "timer.h"

struct process *procs;               /* dynamically-sized process table */
volatile unsigned int current_pid;

static unsigned int proc_cap;            /* current capacity of procs[] */
static unsigned int next_pid_counter = 1; /* monotonic, wraps */
static unsigned int fallback_stack_next;  /* 0..PROC_FALLBACK_STACK_MAX */

/* Top of the in-image bootstrap stack (see kernel.ld). */
extern char __boot_stack_top[];

#define INITIAL_PROC_CAP 16

/* switch.S addresses the table entries with an immediate PCB_SIZE/PCB_CPU_TICKS
 * and cannot see this struct; keep the two in sync. */
_Static_assert(sizeof(struct process) == 116,
	       "struct process layout changed: update PCB_SIZE/PCB_CPU_TICKS in switch.S");

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

/* Fallback pool: a static array, so its address is not a fixed memory-map
 * entry (same reasoning as the in-image stack in kernel.ld). */
static unsigned int fallback_stacks[PROC_FALLBACK_STACK_MAX][PROC_STACK_SIZE / 4]
	__attribute__((aligned(16)));

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
		base = (unsigned int)&fallback_stacks[fallback_stack_next][0];
		fallback_stack_next++;
		*owned = 0;
		return base;
	}
	return 0;
}

/* Clear one control block.  kmalloc memory is not zeroed and QEMU hides that
 * (guest RAM starts cleared), so a fresh slot would otherwise carry garbage in
 * cr3/pid/state; schedule() and process_map_mmio_all() trust those fields and
 * a garbage cr3 made the boot-time MMIO mapping fault on real hardware. */
static void clear_proc(struct process *p)
{
	unsigned char *b = (unsigned char *)p;
	unsigned int i;

	for (i = 0; i < sizeof(*p); i++)
		b[i] = 0;
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
		clear_proc(&np[i]);
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
		clear_proc(&procs[i]);

	procs[0].state      = PROC_RUNNING;
	procs[0].pid        = 0;
	procs[0].esp        = 0;
	procs[0].screen     = (void *)0;
	procs[0].stack_base = 0;  /* bootstrap stack, never freed */
	procs[0].cr3        = paging_current_cr3();
	procs[0].kstack_top = (unsigned int)__boot_stack_top;
	procs[0].cpu_ticks  = 0;
	procs[0].start_ticks = 0;  /* the WM "starts" at boot */
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
	unsigned int base, top, *sp, arg_ptr = 0, cr3;
	unsigned int flags;

	flags = irq_save_disable();

	slot = alloc_slot();
	if (slot < 0) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		mm_free_page(cr3);
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
	procs[slot].cr3        = cr3;
	procs[slot].kstack_top = top;
	procs[slot].cpu_ticks  = 0;
	procs[slot].start_ticks = get_ticks_ms();
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
	unsigned int base, top, *sp, cr3;
	unsigned int flags;

	flags = irq_save_disable();

	slot = alloc_slot();
	if (slot < 0) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		mm_free_page(cr3);
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
	procs[slot].cr3        = cr3;
	procs[slot].kstack_top = top;
	procs[slot].cpu_ticks  = 0;
	procs[slot].start_ticks = get_ticks_ms();
	name_copy(procs[slot].name, name);
	procs[slot].screen = procs[current_pid].screen;
	irq_restore(flags);
	return (int)procs[slot].pid;
}

/* Load an ELF image into a fresh address space and run it as a ring-3 process. */
int process_create_user(const char *name, const void *image, unsigned int size,
			const char *arg, const char *cwd)
{
	int slot, owned;
	unsigned int base, top, *sp, cr3, entry;
	unsigned int flags;
	int i;

	flags = irq_save_disable();

	slot = alloc_slot();
	if (slot < 0) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		irq_restore(flags);
		return -1;
	}

	entry = elf_load(cr3, image, size);
	if (!entry) {
		mm_free_page(cr3);
		irq_restore(flags);
		return -2;   /* not a loadable ELF */
	}

	/* Map the user stack just below USER_STACK_TOP.  Keep the top page's
	 * physical address: the argument string is written through it (identity
	 * mapped) after it is mapped in `cr3`. */
	{
		unsigned int top_phys = 0;
		unsigned int user_esp = USER_STACK_TOP;

		for (i = 0; i < USER_STACK_PAGES; i++) {
			unsigned int phys = mm_alloc_page();
			unsigned int va = USER_STACK_TOP - (unsigned int)(i + 1) * 0x1000;
			if (!phys ||
			    !paging_map_page(cr3, va, phys,
					     PTE_USER | PTE_WRITABLE | PTE_PRESENT)) {
				mm_free_page(cr3);
				irq_restore(flags);
				return -1;
			}
			if (i == 0)
				top_phys = phys;
		}

		/* One C string argument at the top of the user stack with a fake
		 * return address below it, so `_start(const char *arg)` reads it
		 * as its first parameter without needing a crt0.  The virtual
		 * addresses go into the frame; the writes go through the identity
		 * map at the same offset from the physical page top. */
		{
			unsigned int va_top = USER_STACK_TOP;
			unsigned char *base_phys = (unsigned char *)(top_phys + 0x1000);
			unsigned char *str_phys;
			unsigned int *frame_phys;
			unsigned int len = 0;
			unsigned int va, k;

			if (arg)
				while (arg[len])
					len++;
			va = (va_top - (len + 1)) & ~15u;
			if (va - 8 < USER_STACK_TOP - USER_STACK_PAGES * 0x1000) {
				mm_free_page(cr3);
				irq_restore(flags);
				return -1;   /* argument does not fit the stack */
			}
			str_phys = base_phys - (va_top - va);
			for (k = 0; k <= len; k++)
				str_phys[k] = arg ? arg[k] : '\0';
			frame_phys = (unsigned int *)(str_phys - 8);
			frame_phys[0] = 0;   /* fake return address */
			frame_phys[1] = va;  /* the argument pointer (user VA) */
			user_esp = va - 8;
		}
		base = stack_alloc(&owned);
		if (!base) {
			mm_free_page(cr3);
			irq_restore(flags);
			return -1;
		}
		top = base + PROC_STACK_SIZE;
		sp = (unsigned int *)top;
		*--sp = 0x23;                 /* SS: user data */
		*--sp = user_esp;             /* ESP: user stack + arg frame */
		*--sp = 0x202;                /* EFLAGS: IF=1 */
		*--sp = 0x1B;                 /* CS: user code */
		*--sp = entry;                /* EIP */
		*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* EAX ECX EDX EBX */
		*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* ESP EBP ESI EDI */
	}

	procs[slot].esp        = (unsigned int)sp;
	procs[slot].state      = PROC_READY;
	procs[slot].pid        = next_pid_counter++;
	procs[slot].stack_base = owned ? base : 0;
	procs[slot].cr3        = cr3;
	procs[slot].kstack_top = top;
	procs[slot].cpu_ticks  = 0;
	procs[slot].start_ticks = get_ticks_ms();
	name_copy(procs[slot].name, name);
	procs[slot].screen = procs[current_pid].screen;
	{
		unsigned int k;

		for (k = 0; cwd && cwd[k] && k + 1 < sizeof(procs[slot].cwd); k++)
			procs[slot].cwd[k] = cwd[k];
		procs[slot].cwd[k] = '\0';
	}

	irq_restore(flags);
	return (int)procs[slot].pid;
}

/* Set the output screen of the currently-running process (used by a terminal
   at startup to bind itself to its own screen). */
void process_set_screen(struct text_screen *screen)
{
	procs[current_pid].screen = screen;
}

void process_map_mmio_all(unsigned int phys_base, unsigned int size)
{
	unsigned int i, kernel_cr3 = paging_current_cr3();

	paging_map_mmio(phys_base, size);
	for (i = 0; i < proc_cap; i++) {
		if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD)
			continue;
		if (procs[i].cr3 && procs[i].cr3 != kernel_cr3)
			paging_map_mmio_cr3(procs[i].cr3, phys_base, size);
	}
}

void process_exit(void)
{
	procs[current_pid].state = PROC_DEAD;
	__asm__ volatile ("sti");  /* re-enable IRQs so the PIT can switch us out */
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
			sys_net_reap(procs[i].pid);   /* release a held NIC */
			if (procs[i].stack_base)
				mm_free_page(procs[i].stack_base);
			if (procs[i].cr3 && procs[i].cr3 != paging_current_cr3())
				mm_free_page(procs[i].cr3);
			procs[i].stack_base = 0;
			procs[i].cr3 = 0;
			procs[i].state = PROC_UNUSED;
		}
	}

	/* A file syscall is in progress: stay on the current process so the FAT
	 * driver's static state is never touched concurrently (see sys_fs.c).
	 * Interrupts stay enabled, so the disk's IRQs are still serviced. */
	if (sys_fs_busy) {
		if (procs[current_pid].state == PROC_READY)
			procs[current_pid].state = PROC_RUNNING;
		return;
	}

	/* Wake sleeping processes whose deadline has passed. */
	for (i = 0; i < proc_cap; i++) {
		if (procs[i].state == PROC_SLEEPING &&
		    (int)(procs[i].wake_ms - get_ticks_ms()) <= 0)
			procs[i].state = PROC_READY;
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

/* Count the 4 KB user pages mapped between USER_BASE and USER_STACK_TOP.
 * A read-only page-directory walk (paging keeps everything identity-mapped,
 * so the tables are directly addressable).  Used by `top` for the memory
 * hint; ring-3 processes currently leak these pages on reap (known issue),
 * but ex-processes are gone from the table by then anyway. */
static unsigned int count_user_pages(unsigned int cr3)
{
	const unsigned int *pd;
	unsigned int i, pages = 0;

	if (!cr3)
		return 0;
	pd = (const unsigned int *)cr3;
	for (i = USER_BASE >> 22; i <= (USER_STACK_TOP - 1) >> 22; i++) {
		unsigned int pde = pd[i];
		const unsigned int *pt;

		if (!(pde & PTE_PRESENT) || (pde & 0x80))  /* 0x80: 4 MB page */
			continue;
		pt = (const unsigned int *)(pde & 0xFFFFF000u);
		{
			unsigned int j;
			for (j = 0; j < 1024; j++)
				if (pt[j] & PTE_PRESENT)
					pages++;
		}
	}
	return pages;
}

/* Snapshot of the index-th live process, for the `top` syscall.  Indexes over
 * live processes in slot order, so callers iterate 0, 1, ... until 0. */
int process_procinfo(unsigned int index, struct sys_procinfo *out)
{
	unsigned int i, seen = 0, life;

	if (!out)
		return 0;
	for (i = 0; i < proc_cap; i++) {
		if (procs[i].state == PROC_UNUSED || procs[i].state == PROC_DEAD)
			continue;
		if (seen++ != index)
			continue;

		life = get_ticks_ms() - procs[i].start_ticks;
		out->pid        = procs[i].pid;
		out->state      = procs[i].state;
		out->cpu_ticks  = procs[i].cpu_ticks;
		out->life_ticks = life ? life : 1;
		/* Kernel stack page + page directory + user pages (the WM shares
		 * the kernel directory; counting one is close enough). */
		out->pages      = 2 + count_user_pages(procs[i].cr3);
		name_copy(out->name, procs[i].name);
		return 1;
	}
	return 0;
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

/* Return 1 if a process with `pid` is still alive (READY/RUNNING/SLEEPING). */
int process_is_alive(unsigned int pid)
{
	unsigned int i;
	for (i = 0; i < proc_cap; i++)
		if (procs[i].pid == pid &&
		    (procs[i].state == PROC_READY ||
		     procs[i].state == PROC_RUNNING ||
		     procs[i].state == PROC_SLEEPING))
			return 1;
	return 0;
}

/* Block the current process for `ms` milliseconds.  Marks it sleeping and
 * yields to the scheduler; it is woken when its deadline passes. */
void process_sleep_ms(unsigned int ms)
{
	procs[current_pid].wake_ms = get_ticks_ms() + ms;
	procs[current_pid].state = PROC_SLEEPING;
	__asm__ volatile ("sti");  /* re-enable IRQs so the PIT can switch us out */
	while (procs[current_pid].state == PROC_SLEEPING)
		cpu_idle();
}

/* Wait (yield) until the process with `pid` has exited. */
void process_wait(unsigned int pid)
{
	while (process_is_alive(pid))
		process_sleep_ms(10);
}
