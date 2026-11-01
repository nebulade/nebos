/* Scheduler core: per-CPU state, the running process, and the tick entry.
 *
 * Only CPU 0 exists today, but everything is shaped for more CPUs later:
 * current is per-CPU, reached through a per-CPU GS segment, and the timer
 * path goes through sched_tick_cpu-style code rather than a global pid index.
 */
#ifndef SCHED_H
#define SCHED_H

#define MAX_CPUS 8

#define SCHED_WEIGHT_DEFAULT 8
#define SCHED_WEIGHT_MAX     16
#define SCHED_SLICE_TICKS    10   /* PIT ticks (1 kHz) per timeslice */

struct process;
struct tss;
struct sys_schedinfo;

struct wait_queue {
	struct process *head;
	struct process *tail;
};

struct cpu {
	struct process *current;   /* offset 0, read by switch.S (%gs:0) */
	struct process *idle;
	struct tss     *tss;       /* this CPU's TSS; schedule() updates esp0 */
	struct cpu     *self;      /* this_cpu() reads it through %gs */
	unsigned int    id;
	unsigned int    lapic_id;
	unsigned int    need_resched;
	unsigned int    preempt_count;
	unsigned int    idle_ticks;
	unsigned int    switches;
};

extern struct cpu cpus[MAX_CPUS];
extern unsigned int cpu_count;
extern unsigned int cpu_gs_ready;

/* The GS descriptor points at this CPU's struct cpu.  Before the GDT has
 * been loaded (early boot, or a fault before gdt_tss_init), fall back to
 * the BSP entry so panic/console code can still run safely. */
static inline struct cpu *this_cpu(void)
{
	struct cpu *c;

	if (!cpu_gs_ready)
		return &cpus[0];
	__asm__ volatile ("movl %%gs:%c1, %0"
			  : "=r"(c)
			  : "i"(__builtin_offsetof(struct cpu, self)));
	return c;
}

static inline struct process *current_proc(void)
{
	return this_cpu()->current;
}

void sched_init(void);
void schedule(void);
int  sched_tick_bsp(void);

/* Voluntary reschedule through the int 0x81 gate.  Must not be called with
 * preempt_count set; blocking helpers below handle that. */
void sched_yield(void);

/* Block the current process until its deadline, or add it to a ready queue.
 * Both are the entry points process.c uses. */
void sched_sleep_ms(unsigned int ms);
void sched_add_ready(struct process *p);
void sched_unlink(struct process *p);

/* Wait queues.  wait_wake_* may be called from any process context; the
 * block helpers handle interrupts themselves. */
void wait_queue_init(struct wait_queue *wq);
void sched_block_on(struct wait_queue *wq);
void sched_block_on_locked(struct wait_queue *wq);
void sched_block_timeout(struct wait_queue *wq, unsigned int ms);
void wait_wake_one(struct wait_queue *wq);
void wait_wake_all(struct wait_queue *wq);

/* Use with interrupts disabled.  sched_set_weight_locked accounts the current
 * slice with the old weight before changing the stride. */
void sched_set_weight_locked(struct process *p, unsigned int weight);

/* Per-CPU non-preemptible sections (the FAT driver and the shadow flush).
 * The PIT still increments ticks and wakes sleepers, but does not switch. */
void preempt_disable(void);
void preempt_enable(void);

/* Snapshot for SYS_SCHEDINFO and the `sched` command. */
int  sched_snapshot(struct sys_schedinfo *out);
void sched_print(void);

#endif
