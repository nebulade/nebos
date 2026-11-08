/* Process objects and lifecycle.  Scheduling itself lives in sched.c. */
#ifndef PROCESS_H
#define PROCESS_H

#include "sched.h"

#define PROC_STACK_SIZE 0x1000  /* 4 KB per process (= 1 page) */

/* Process stacks are allocated from the frame allocator (mm.c), i.e. above the
 * VGA shadow buffer.  If a page is unavailable we fall back to a small static
 * pool in BSS so the system still boots with fewer processes. */
#define PROC_FALLBACK_STACK_MAX  8

/* Stable process table: slots hold pointers to individually allocated PCBs, so
 * a process pointer never moves.  Keep the table small; PCBs are heap objects. */
#define PROC_MAX 64

#define PROC_UNUSED  0
#define PROC_READY   1
#define PROC_RUNNING 2
#define PROC_DEAD    3
#define PROC_SLEEPING 4
#define PROC_BLOCKED 5

/* on_cpu when the process is not currently running anywhere. */
#define PROC_NO_CPU 0xFFFFFFFFu

#define TIMESLICE_TICKS 20 /* context switch every 20 ms (PIT @ 1 kHz) */

#define PROC_NAME_LEN 12
#define PROC_CWD_MAX  64

struct sys_procinfo; /* forward decl (see syscall.h; `top` snapshot) */
struct surface;      /* forward decl (see vga.h; render target override) */

struct process {
	unsigned int esp;            /* offset 0: switch.S saves it here */
	unsigned int state;
	unsigned int pid;
	unsigned int slot;           /* index in procs[], PROC_MAX for idle */
	char         name[PROC_NAME_LEN];
	unsigned int stack_base;     /* page to free on exit, or 0 (bootstrap/fallback) */
	unsigned int cr3;            /* physical address of this process's page dir */
	unsigned int heap_pages;     /* pages mapped at USER_HEAP_BASE, or 0 */
	unsigned int kstack_top;     /* ring-0 stack top (for TSS esp0) */
	unsigned int wake_ms;        /* tick deadline when PROC_SLEEPING */
	char cwd[PROC_CWD_MAX];      /* working directory (ring-3 commands) */
	/* CPU accounting for `top`. */
	unsigned int cpu_ticks;      /* ticks spent as the running process */
	unsigned int start_ticks;    /* get_ticks_ms() at creation */
	/* Render target override: 0 = the screen.  Set while the window manager
	 * draws into a layer, so another process's drawing can never land in
	 * it. */
	struct surface *target;
	/* TTY output id inherited by children (0 = none, use the old screen). */
	int tty;
	/* Per-CPU scaffolding: these are maintained even with one CPU so the
	 * scheduler can be extended without reshaping the PCB. */
	unsigned int on_cpu;         /* PROC_NO_CPU when not running */
	unsigned int last_cpu;
	unsigned int affinity;       /* CPU bitmask; all CPUs by default */
	/* CFS-lite scheduling entity.  vruntime/stride are 64-bit; no 64-bit
	 * division happens on the tick path (stride is computed in set_weight). */
	unsigned int weight;         /* 1..SCHED_WEIGHT_MAX */
	unsigned int on_runq;        /* linked in the ready queue */
	unsigned long long vruntime;
	unsigned long long stride;
	unsigned int slice_start;    /* tick when the current slice began */
	struct process *run_next, *run_prev;
	struct process *sleep_next, *sleep_prev;
	struct process *wait_next, *wait_prev;
	struct wait_queue *waiting_on;
	struct process *zombie_next;    /* reaped by the kworker */
	unsigned int zombie_queued;
	/* Kernel-thread entry: set for processes created with
	 * process_create*(), whose wrapper takes the BKL. */
	void (*kentry)(void);
	unsigned int kernel_thread;
	/* BKL depth saved when the process blocks and must reacquire it. */
	unsigned int bkl_saved;
};

/* User-mode virtual address layout (ring-3 processes). */
#define USER_BASE        0x40000000   /* user program link/load address */
#define USER_STACK_TOP   0x40200000   /* user stack top (grows down) */
#define USER_STACK_PAGES 8            /* user stack size in 4 KB pages */
/* Where a canvas window's back buffer is mapped.  The mapping is released by
 * the reaper without freeing the canvas pages: the window owns those. */
#define USER_WIN_BASE    0x50000000
#define USER_WIN_RANGE   0x00400000   /* one 4 MB slot */
/* Anonymous per-process heap (SYS_MEM_ALLOC/GROW/FREE): one growable arena
 * above the WM mappings.  Freed by the reaper. */
#define USER_HEAP_BASE   0x60000000u
#define USER_HEAP_MAX    (32u * 1024u * 1024u)

/* Stable, sparse process table (see process.c). */
extern struct process *procs[PROC_MAX];

void process_init(void);
/* Prepare an AP idle PCB and stack; returns its kernel stack top, or 0. */
unsigned int process_init_ap(unsigned int cpu);
int  process_create(const char *name, void (*entry)(void));
int  process_create_arg(const char *name, void (*entry)(const char *),
			const char *arg);
int  process_create_ptr(const char *name, void (*entry)(void *), void *arg);
/* Load an ELF image into a fresh address space and run it as a ring-3
 * process, with `arg` (may be NULL) as the first C parameter of _start.
 * Returns the pid, -1 when out of resources, or -2 when the image is not a
 * loadable ELF. */
int process_create_user(const char *name, const void *image, unsigned int size,
			const char *arg, const char *cwd);
/* Same, but with an explicit tty id (0 = inherit the caller's tty). */
int process_create_user_tty(const char *name, const void *image,
			unsigned int size, const char *arg, const char *cwd,
			unsigned int tty);
/* Render target override for the running process (0 = the screen). */
struct surface *process_render_target(void);
void process_set_render_target(struct surface *s);

/* Map an MMIO range into the kernel's page directory and every live process
 * copy (and thus every process created later, which clones the kernel map).
 * Drivers that map their register window at command time must use this: a
 * process runs with its own page-directory copy, so paging_map_mmio() alone
 * would only be visible to the kernel bootstrap context. */
void process_map_mmio_all(unsigned int phys_base, unsigned int size);
void process_exit(void);
/* Called by the kworker: free switched-out dead processes. */
void process_reap_dead(void);
unsigned int process_zombie_count(void);
/* Caller holds sched_lock. */
unsigned int process_zombie_count_locked(void);
/* Called from schedule()/kill with interrupts disabled: unpublish a dead PCB
 * and put it on the kworker's zombie list. */
void process_zombie_add(struct process *p);
void process_ps(void);

/* Fill *out with a snapshot of the index-th live process (slot order) for
 * the `top` command.  Returns 1 when found, 0 at the end of the table. */
int  process_procinfo(unsigned int index, struct sys_procinfo *out);
int  process_kill(unsigned int pid);
int  process_is_alive(unsigned int pid);
/* 1 while the process is in PROC_SLEEPING (the WM uses this to tell when it
 * is safe to reap another window's canvas). */
int  process_is_sleeping(unsigned int pid);
void process_set_tty(unsigned int pid, int tty);
void process_wait(unsigned int pid);
/* Adjust the CFS weight of a live process (1..SCHED_WEIGHT_MAX). */
int  process_set_weight(unsigned int pid, unsigned int weight);
/* Restrict a live process to a CPU bitmask (0 is ignored). */
int  process_set_affinity(unsigned int pid, unsigned int mask);

/* Block the current process for `ms` milliseconds (yields the CPU). */
void process_sleep_ms(unsigned int ms);

#endif
