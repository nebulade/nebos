/* Process management: create, exit, reap, ps, kill.
 *
 * The scheduler itself lives in sched.c.  This file owns the stable PCB
 * objects and the lifecycle around them: allocation, initial stack frames,
 * address-space cleanup, and the user-facing helpers.
 */

#include "process.h"
#include "elf.h"
#include "idle.h"
#include "io.h"
#include "kmalloc.h"
#include "mm.h"
#include "paging.h"
#include "serial.h"
#include "smp.h"
#include "sys_fs.h"
#include "sys_gfx.h"
#include "sys_mem.h"
#include "sys_net.h"
#include "syscall.h"
#include "timer.h"
#include "tty.h"
#include "vga.h"
#include "wm_abi.h"

struct process *procs[PROC_MAX];

static struct process idle_procs[MAX_CPUS];
static unsigned int next_pid_counter = 1; /* monotonic, wraps */
static unsigned int fallback_stack_next;  /* 0..PROC_FALLBACK_STACK_MAX */
static struct process *zombie_head, *zombie_tail;
static struct wait_queue exit_wq;

/* Top of the in-image bootstrap stack (see kernel.ld). */
extern char __boot_stack_top[];

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
 * (guest RAM starts cleared), so a fresh PCB would otherwise carry garbage in
 * cr3/pid/state; the scheduler and MMIO mapping code trust those fields. */
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

static void cwd_copy(struct process *p, const char *cwd)
{
	unsigned int i;

	for (i = 0; cwd && cwd[i] && i + 1 < sizeof(p->cwd); i++)
		p->cwd[i] = cwd[i];
	p->cwd[i] = '\0';
}

/* Find a free slot in the stable pointer table.  Called with interrupts
 * disabled.  Returns the slot index, or -1 when the table is full. */
static int alloc_slot(void)
{
	unsigned int slot;

	for (slot = 0; slot < PROC_MAX; slot++)
		if (!procs[slot])
			return (int)slot;
	return -1;
}

/* Allocate and clear a PCB, then reserve a table slot (but do not publish it
 * yet).  Called with interrupts disabled. */
static struct process *proc_alloc(int *slot_out)
{
	struct process *p = (struct process *)kmalloc(sizeof(*p));
	int slot;

	if (!p)
		return 0;
	clear_proc(p);
	slot = alloc_slot();
	if (slot < 0) {
		kfree(p);
		return 0;
	}
	*slot_out = slot;
	return p;
}

/* Fill in the common fields and publish the process in its slot.  The caller
 * has already built the initial kernel stack frame.  Called with interrupts
 * disabled and with parent = current_proc() (possibly idle). */
static void proc_commit(struct process *p, int slot, const char *name,
			unsigned int esp, int owned, unsigned int stack_base,
			unsigned int cr3, unsigned int kstack_top,
			void (*kentry)(void), int kernel)
{
	struct process *parent = current_proc();

	p->esp = esp;
	p->pid = next_pid_counter++;
	p->slot = (unsigned int)slot;
	p->stack_base = owned ? stack_base : 0;
	p->cr3 = cr3;
	p->kstack_top = kstack_top;
	p->cpu_ticks = 0;
	p->start_ticks = get_ticks_ms();
	p->on_cpu = PROC_NO_CPU;
	p->last_cpu = PROC_NO_CPU;
	/* Kernel threads touch global kernel state; keep them on the BSP.
	 * Ring-3 processes may use every CPU. */
	p->affinity = kernel ? 1u : 0xFFFFFFFFu;
	p->kentry = kentry;
	p->kernel_thread = (unsigned int)kernel;
	p->weight = 0;
	p->stride = 0;
	p->vruntime = 0;
	p->slice_start = 0;
	p->screen = parent ? parent->screen : (void *)0;
	p->tty = parent ? parent->tty : 0;
	if (parent)
		cwd_copy(p, parent->cwd);
	else
		p->cwd[0] = '\0';
	name_copy(p->name, name);
	procs[slot] = p;
	/* The caller publishes the process as runnable only after applying any
	 * per-process overrides (cwd/tty): an AP can pick it as soon as it is
	 * on the run queue. */
}

void process_init(void)
{
	unsigned int i;

	sched_init();
	zombie_head = zombie_tail = 0;
	wait_queue_init(&exit_wq);
	for (i = 0; i < PROC_MAX; i++)
		procs[i] = 0;

	clear_proc(&idle_procs[0]);
	idle_procs[0].state      = PROC_RUNNING;
	idle_procs[0].pid        = 0;
	idle_procs[0].slot       = PROC_MAX;
	idle_procs[0].esp        = 0;
	idle_procs[0].screen     = (void *)0;
	idle_procs[0].stack_base = 0;  /* bootstrap stack, never freed */
	idle_procs[0].cr3        = paging_current_cr3();
	idle_procs[0].kstack_top = (unsigned int)__boot_stack_top;
	idle_procs[0].cpu_ticks  = 0;
	idle_procs[0].start_ticks = 0;
	idle_procs[0].on_cpu     = 0;
	idle_procs[0].last_cpu   = 0;
	idle_procs[0].affinity   = 1;
	idle_procs[0].weight     = 0;
	idle_procs[0].stride     = 0;
	idle_procs[0].vruntime   = 0;
	idle_procs[0].on_runq    = 0;
	name_copy(idle_procs[0].name, "idle");

	cpus[0].idle = &idle_procs[0];
	cpus[0].current = &idle_procs[0];
}

/* Prepare an AP idle process and its kernel stack.  Called by the BSP before
 * SIPI; the AP itself publishes cpus[cpu].current in smp_ap_main(). */
unsigned int process_init_ap(unsigned int cpu)
{
	struct process *p;
	unsigned int base;

	if (cpu == 0 || cpu >= MAX_CPUS)
		return 0;
	base = mm_alloc_page();
	if (!base)
		return 0;
	p = &idle_procs[cpu];
	clear_proc(p);
	p->state      = PROC_RUNNING;
	p->pid        = 0;
	p->slot       = PROC_MAX;
	p->esp        = 0;
	p->cr3        = paging_kernel_cr3();
	p->kstack_top = base + PROC_STACK_SIZE;
	p->on_cpu     = cpu;
	p->last_cpu   = cpu;
	p->affinity   = 1u << cpu;
	p->start_ticks = 0;
	name_copy(p->name, "idle");
	cpus[cpu].idle = p;
	return p->kstack_top;
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
/* Kernel-thread entry wrappers: take the BKL for the thread's lifetime and
 * hand the real entry point the argument the stack frame supplied. */
static void process_kernel_start0(void)
{
	struct process *p = current_proc();
	void (*fn)(void) = (void (*)(void))p->kentry;

	bkl_lock();
	if (fn)
		fn();
	bkl_unlock();
	process_exit();
}

static void process_kernel_start_arg(const char *arg)
{
	struct process *p = current_proc();
	void (*fn)(const char *) = (void (*)(const char *))p->kentry;

	bkl_lock();
	if (fn)
		fn(arg);
	bkl_unlock();
	process_exit();
}

static int setup_process(const char *name, unsigned int entry,
			 int has_arg, const char *arg,
			 void (*kentry)(void), int kernel)
{
	int slot, owned, pid;
	unsigned int base, top, *sp, arg_ptr = 0, cr3;
	unsigned int flags;
	struct process *p;

	flags = irq_save_disable();

	p = proc_alloc(&slot);
	if (!p) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		kfree(p);
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		mm_free_page(cr3);
		kfree(p);
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

	proc_commit(p, slot, name, (unsigned int)sp, owned, base, cr3, top,
		    kentry, kernel);
	sched_add_ready(p);
	pid = (int)p->pid;
	irq_restore(flags);
	return pid;
}

int process_create(const char *name, void (*entry)(void))
{
	return setup_process(name, (unsigned int)process_kernel_start0, 0,
			     (void *)0, entry, 1);
}

int process_create_arg(const char *name, void (*entry)(const char *),
		       const char *arg)
{
	return setup_process(name, (unsigned int)process_kernel_start_arg, 1,
			     arg, (void (*)(void))entry, 1);
}

/* Create a process whose entry takes a raw pointer argument (no string copy). */
int process_create_ptr(const char *name, void (*entry)(void *), void *arg)
{
	int slot, owned, pid;
	unsigned int base, top, *sp, cr3;
	unsigned int flags;
	struct process *p;

	flags = irq_save_disable();

	p = proc_alloc(&slot);
	if (!p) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		kfree(p);
		irq_restore(flags);
		return -1;
	}

	base = stack_alloc(&owned);
	if (!base) {
		mm_free_page(cr3);
		kfree(p);
		irq_restore(flags);
		return -1;
	}
	top = base + PROC_STACK_SIZE;
	sp = (unsigned int *)top;

	*--sp = (unsigned int)arg;        /* first C parameter */
	*--sp = (unsigned int)process_exit;/* return address */
	*--sp = 0x202;                    /* EFLAGS: IF=1 */
	*--sp = 0x08;                     /* CS */
	*--sp = (unsigned int)process_kernel_start_arg; /* EIP */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* EAX ECX EDX EBX */
	*--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  /* ESP EBP ESI EDI */

	proc_commit(p, slot, name, (unsigned int)sp, owned, base, cr3, top,
		    (void (*)(void))entry, 1);
	sched_add_ready(p);
	pid = (int)p->pid;
	irq_restore(flags);
	return pid;
}

/* Load an ELF image into a fresh address space and run it as a ring-3 process. */
int process_create_user_tty(const char *name, const void *image, unsigned int size,
			    const char *arg, const char *cwd, unsigned int tty)
{
	int slot, owned, pid;
	unsigned int base, top, *sp, cr3, entry;
	unsigned int flags;
	struct process *p;
	int i;

	flags = irq_save_disable();

	p = proc_alloc(&slot);
	if (!p) {
		irq_restore(flags);
		return -1;
	}

	cr3 = paging_new_directory();
	if (!cr3) {
		kfree(p);
		irq_restore(flags);
		return -1;
	}

	entry = elf_load(cr3, image, size);
	if (!entry) {
		mm_free_page(cr3);
		kfree(p);
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
				kfree(p);
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
				kfree(p);
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
			kfree(p);
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

	proc_commit(p, slot, name, (unsigned int)sp, owned, base, cr3, top,
		    0, 0);
	if (cwd)
		cwd_copy(p, cwd);
	if (tty)
		p->tty = (int)tty;
	sched_add_ready(p);
	pid = (int)p->pid;

	irq_restore(flags);
	return pid;
}

int process_create_user(const char *name, const void *image, unsigned int size,
			const char *arg, const char *cwd)
{
	return process_create_user_tty(name, image, size, arg, cwd, 0);
}

/* Set the output screen of the currently-running process (used by a terminal
   at startup to bind itself to its own screen). */
void process_set_screen(struct text_screen *screen)
{
	struct process *p = current_proc();

	if (p)
		p->screen = screen;
}

/* Render target override for the running process (0 = the screen).  Lives in
 * the PCB so a preempted layer draw can never leak into another process. */
struct surface *process_render_target(void)
{
	struct process *p = current_proc();

	return p ? p->target : 0;
}

void process_set_render_target(struct surface *s)
{
	struct process *p = current_proc();

	if (p)
		p->target = s;
}

void process_map_mmio_all(unsigned int phys_base, unsigned int size)
{
	unsigned int i, flags;

	/* Map the kernel directory (the one new process directories clone) and
	 * every live process copy.  paging_current_cr3() is the *current* process
	 * directory, which may be one of these; mapping it twice is harmless.
	 * The reaper frees PCBs, so hold the process lock while iterating. */
	flags = sched_lock_acquire();
	paging_map_mmio(phys_base, size);
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (!p || p->state == PROC_DEAD)
			continue;
		if (p->cr3)
			paging_map_mmio_cr3(p->cr3, phys_base, size);
	}
	sched_lock_release(flags);
}

/* Unpublish a dead process and hand it to the kworker.  Takes sched_lock
 * itself; callers must have already switched away from its kernel stack (or
 * never run it), so the reaper cannot free memory still in use. */
void process_zombie_add(struct process *p)
{
	unsigned int flags;

	if (!p)
		return;
	flags = sched_lock_acquire();
	if (p->zombie_queued) {
		sched_lock_release(flags);
		return;
	}
	p->zombie_queued = 1;
	p->on_cpu = PROC_NO_CPU;
	if (p->slot < PROC_MAX)
		procs[p->slot] = 0;
	p->zombie_next = 0;
	if (zombie_tail)
		zombie_tail->zombie_next = p;
	else
		zombie_head = p;
	zombie_tail = p;
	sched_lock_release(flags);
}

unsigned int process_zombie_count_locked(void)
{
	unsigned int n = 0;
	struct process *p;

	for (p = zombie_head; p; p = p->zombie_next)
		n++;
	return n;
}

unsigned int process_zombie_count(void)
{
	unsigned int flags, n;

	flags = sched_lock_acquire();
	n = process_zombie_count_locked();
	sched_lock_release(flags);
	return n;
}

/* Free switched-out dead processes.  Runs in the kworker (or the idle
 * fallback), not in the PIT handler.  Preemption is disabled while the
 * resources are torn down: sys_gfx_reap()/tty_reap()/the page-table walk are
 * not re-entrant, matching the old IRQ-context reaper. */
void process_reap_dead(void)
{
	unsigned int cur_cr3 = paging_current_cr3();
	unsigned int flags;
	struct process *p, *list;

	preempt_disable();
	flags = sched_lock_acquire();
	list = zombie_head;
	zombie_head = zombie_tail = 0;
	sched_lock_release(flags);

	for (p = list; p; ) {
		struct process *next = p->zombie_next;

		sys_net_reap(p->pid);   /* release a held NIC */
		sys_gfx_reap(p->pid);   /* flag a gfx session dead */
		tty_reap(p->pid);       /* drop its terminal queue */
		if (p->stack_base)
			mm_free_page(p->stack_base);
		if (p->cr3 && p->cr3 != cur_cr3) {
			/* User code/stack pages belong to the process.  The gfx
			 * canvas mapping is only a window into pages owned by
			 * the session: release the PTEs and page tables, not
			 * the pages. */
			paging_release_user_range(p->cr3,
				USER_BASE, USER_STACK_TOP - USER_BASE, 1);
			paging_release_user_range(p->cr3,
				USER_GFX_BASE, USER_GFX_RANGE, 0);
			sys_mem_release(p);
			/* WM surface mappings: page tables only; the shadow/
			 * terminal/canvas pages belong to the window server. */
			paging_release_user_range(p->cr3,
				WM_SHADOW_VA, 0x00800000, 0);
			paging_release_user_range(p->cr3,
				WM_CONTENT_VA_BASE,
				WM_CONTENT_SLOT * WM_MAX_WINDOWS, 0);
			mm_free_page(p->cr3);
		}
		p->stack_base = 0;
		p->cr3 = 0;
		kfree(p);
		p = next;
	}
	preempt_enable();
}

void process_exit(void)
{
	struct process *p = current_proc();

	if (p)
		p->state = PROC_DEAD;
	wait_wake_all(&exit_wq);
	sched_yield();          /* switch away immediately via int 0x81 */
	for (;;)
		__asm__ volatile ("hlt");  /* the scheduler never resumes us */
}

/* ---- user-facing helpers ---- */

static const char *state_str(unsigned int s)
{
	switch (s) {
	case PROC_RUNNING:  return "run  ";
	case PROC_READY:    return "ready";
	case PROC_SLEEPING: return "sleep";
	case PROC_BLOCKED:  return "block";
	default:            return "?    ";
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
	unsigned int i, j;
	io_putstr("PID  STATE  W CPU NAME\n");
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p;
		unsigned int pid, state, weight, cpu, flags;
		char name[PROC_NAME_LEN];

		/* The reaper frees PCBs; snapshot this slot under the process
		 * lock and print the copy outside the critical section. */
		flags = sched_lock_acquire();
		p = procs[i];
		if (!p || p->state == PROC_DEAD) {
			sched_lock_release(flags);
			continue;
		}
		pid = p->pid;
		state = p->state;
		weight = p->weight;
		cpu = p->on_cpu;
		for (j = 0; j < sizeof(name) - 1 && p->name[j]; j++)
			name[j] = p->name[j];
		name[j] = '\0';
		sched_lock_release(flags);

		io_putchar(' ');
		put_dec(pid);
		io_putstr("  ");
		io_putstr(state_str(state));
		io_putstr("  ");
		put_dec(weight);
		io_putstr("   ");
		if (cpu != PROC_NO_CPU)
			put_dec(cpu);
		else
			io_putchar('-');
		io_putstr("   ");
		io_putstr(name);
		io_putchar('\n');
	}
}

/* Count the 4 KB user pages mapped between USER_BASE and USER_STACK_TOP.
 * A read-only page-directory walk (paging keeps everything identity-mapped,
 * so the tables are directly addressable).  Used by `top` for the memory
 * hint. */
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
	unsigned int i, seen = 0, life, flags;

	if (!out)
		return 0;
	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (!p || p->state == PROC_DEAD)
			continue;
		if (seen++ != index)
			continue;

		life = get_ticks_ms() - p->start_ticks;
		out->pid        = p->pid;
		out->state      = p->state;
		out->cpu_ticks  = p->cpu_ticks;
		out->life_ticks = life ? life : 1;
		/* Kernel stack page + page directory + user pages (the WM shares
		 * the kernel directory; counting one is close enough). */
		out->pages      = 2 + count_user_pages(p->cr3) + p->heap_pages;
		out->weight     = p->weight;
		out->cpu        = p->on_cpu;
		name_copy(out->name, p->name);
		sched_lock_release(flags);
		return 1;
	}
	sched_lock_release(flags);
	return 0;
}

int process_kill(unsigned int pid)
{
	struct process *target = 0;
	unsigned int i, flags, on_cpu;

	if (pid == 0)
		return -1; /* cannot kill the kernel idle process */
	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->state != PROC_DEAD && p->pid == pid) {
			target = p;
			break;
		}
	}
	if (!target) {
		sched_lock_release(flags);
		return -1;
	}
	if (target == current_proc()) {
		target->state = PROC_DEAD;
		sched_lock_release(flags);
		wait_wake_all(&exit_wq);
		sched_yield();     /* never resumes */
		return 0;
	}
	on_cpu = target->on_cpu;
	sched_unlink_locked(target);
	target->state = PROC_DEAD;
	sched_lock_release(flags);

	wait_wake_all(&exit_wq);

	if (on_cpu == PROC_NO_CPU) {
		/* Not running anywhere: its page directory is no longer in any
		 * CR3, so the reaper may take it now. */
		process_zombie_add(target);
	} else {
		/* The target CPU's next schedule queues the zombie only after it
		 * has switched away from it. */
		smp_resched_cpu(on_cpu);
	}
	return 0;
}

int process_set_weight(unsigned int pid, unsigned int weight)
{
	unsigned int i, flags;

	if (weight < 1)
		weight = 1;
	if (weight > SCHED_WEIGHT_MAX)
		weight = SCHED_WEIGHT_MAX;

	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->state != PROC_DEAD && p->pid == pid) {
			sched_set_weight_locked(p, weight);
			sched_lock_release(flags);
			return 0;
		}
	}
	sched_lock_release(flags);
	return -1;
}

int process_set_affinity(unsigned int pid, unsigned int mask)
{
	unsigned int i, flags;

	if (!mask)
		return -1;
	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->state != PROC_DEAD && p->pid == pid) {
			p->affinity = mask;
			sched_lock_release(flags);
			return 0;
		}
	}
	sched_lock_release(flags);
	return -1;
}

/* Interrupts off. */
static int process_alive_locked(unsigned int pid)
{
	unsigned int i;

	if (pid == 0)
		return 1;   /* idle is not in the table but always exists */
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->pid == pid &&
		    (p->state == PROC_READY ||
		     p->state == PROC_RUNNING ||
		     p->state == PROC_SLEEPING ||
		     p->state == PROC_BLOCKED))
			return 1;
	}
	return 0;
}

/* Return 1 if a process with `pid` is still alive. */
int process_is_alive(unsigned int pid)
{
	unsigned int flags;
	int alive;

	flags = sched_lock_acquire();
	alive = process_alive_locked(pid);
	sched_lock_release(flags);
	return alive;
}

int process_is_sleeping(unsigned int pid)
{
	unsigned int i, flags;
	int sleeping = 0;

	if (pid == 0)
		return 0;
	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->pid == pid) {
			sleeping = (p->state == PROC_SLEEPING);
			break;
		}
	}
	sched_lock_release(flags);
	return sleeping;
}

void process_set_tty(unsigned int pid, int tty)
{
	unsigned int i, flags;

	flags = sched_lock_acquire();
	for (i = 0; i < PROC_MAX; i++) {
		struct process *p = procs[i];

		if (p && p->pid == pid && p->state != PROC_DEAD) {
			p->tty = tty;
			break;
		}
	}
	sched_lock_release(flags);
}

/* Block the current process for `ms` milliseconds.  The sleep list wakes it
 * from the PIT; no polling loop is needed. */
void process_sleep_ms(unsigned int ms)
{
	sched_sleep_ms(ms);
}

/* Wait until the process with `pid` has exited.  The exit path wakes the
 * queue, so this is event-driven rather than a 10 ms poll. */
void process_wait(unsigned int pid)
{
	for (;;) {
		unsigned int flags = sched_lock_acquire();

		if (!process_alive_locked(pid)) {
			sched_lock_release(flags);
			return;
		}
		sched_block_on_locked(&exit_wq);
		sched_lock_release(flags);
		sched_yield();
	}
}
