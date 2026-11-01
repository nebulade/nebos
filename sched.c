/* CFS-lite scheduler core: per-CPU state, a weighted run queue, a sorted sleep
 * list and the timer entry.
 *
 * Weight is an integer 1..SCHED_WEIGHT_MAX.  Each ready process has a 64-bit
 * virtual runtime advanced by (elapsed ticks * stride) while it runs, where
 * stride = SCHED_WEIGHT_SCALE / weight.  The smallest vruntime gets the CPU,
 * so the long-term share is proportional to the weight.  A 10 ms slice bounds
 * latency; a wake-up gets clamped to the current minimum vruntime so an
 * interactive process is scheduled promptly without accumulating unbounded
 * credit.
 *
 * The run queue is a single global list protected by sched_lock; per-CPU
 * current lets every CPU pick the next eligible task from it.
 */

#include "sched.h"
#include "io.h"
#include "lapic.h"
#include "paging.h"
#include "process.h"
#include "smp.h"
#include "spinlock.h"
#include "syscall.h"
#include "thermal.h"
#include "timer.h"
#include "tss.h"

/* 2^20 / weight, so weight 8 advances 131072 per tick and weight 16 half
 * that.  The product fits comfortably in the 64-bit vruntime. */
#define SCHED_WEIGHT_SCALE (1u << 20)

struct cpu cpus[MAX_CPUS];
unsigned int cpu_count;
unsigned int cpu_gs_ready;

static struct process *runq_head, *runq_tail;
static struct process *sleep_head, *sleep_tail;
static unsigned int wakeups;

/* Protects the global run/sleep/wait queues and the process-table state that
 * the timer path touches.  This is a spinlock: it is held with interrupts
 * off for short, non-blocking critical sections only. */
static spinlock_t sched_lock;

/* Sleeping big kernel lock.  One owner process at a time (recursive); the
 * owner is never timer-preempted, and every voluntary block drops it so the
 * other CPU can keep making progress.  See bkl_lock()/bkl_unlock(). */
static struct {
	struct process *owner;
	unsigned int depth;
	struct wait_queue wait;
} bkl;

unsigned int sched_lock_acquire(void)
{
	unsigned int flags = spin_irq_save_disable();

	spin_lock(&sched_lock);
	return flags;
}

void sched_lock_release(unsigned int flags)
{
	spin_unlock(&sched_lock);
	spin_irq_restore(flags);
}

void sched_lock_reset(void)
{
	sched_lock.locked = 0;
}

/* switch.S reads these two facts directly: current at %gs:0 and esp at
 * offset 0 of the process.  Keep them true. */
_Static_assert(__builtin_offsetof(struct cpu, current) == 0,
	       "switch.S reads current from %gs:0");
_Static_assert(__builtin_offsetof(struct process, esp) == 0,
	       "switch.S saves the interrupted esp at offset 0");
_Static_assert(__builtin_offsetof(struct cpu, need_resched) == 24,
	       "smp_resched_wake expects need_resched at offset 24");

static inline unsigned long long weight_stride(unsigned int weight)
{
	if (weight < 1)
		weight = 1;
	if (weight > SCHED_WEIGHT_MAX)
		weight = SCHED_WEIGHT_MAX;
	return (unsigned long long)(SCHED_WEIGHT_SCALE / weight);
}

/* ---- run queue (interrupts off) ---- */

static void runq_add_locked(struct process *p)
{
	if (p->on_runq)
		return;
	p->on_runq = 1;
	p->run_prev = runq_tail;
	p->run_next = 0;
	if (runq_tail)
		runq_tail->run_next = p;
	else
		runq_head = p;
	runq_tail = p;
}

static void runq_del_locked(struct process *p)
{
	if (!p->on_runq)
		return;
	if (p->run_prev)
		p->run_prev->run_next = p->run_next;
	else
		runq_head = p->run_next;
	if (p->run_next)
		p->run_next->run_prev = p->run_prev;
	else
		runq_tail = p->run_prev;
	p->on_runq = 0;
	p->run_next = p->run_prev = 0;
}

static void sleepq_add_locked(struct process *p)
{
	struct process *it = sleep_head, *prev = 0;

	while (it && (int)(it->wake_ms - p->wake_ms) <= 0) {
		prev = it;
		it = it->sleep_next;
	}
	p->sleep_prev = prev;
	p->sleep_next = it;
	if (prev)
		prev->sleep_next = p;
	else
		sleep_head = p;
	if (it)
		it->sleep_prev = p;
	else
		sleep_tail = p;
}

static void sleepq_del_locked(struct process *p)
{
	if (p->sleep_prev)
		p->sleep_prev->sleep_next = p->sleep_next;
	else if (sleep_head == p)
		sleep_head = p->sleep_next;
	if (p->sleep_next)
		p->sleep_next->sleep_prev = p->sleep_prev;
	else if (sleep_tail == p)
		sleep_tail = p->sleep_prev;
	p->sleep_next = p->sleep_prev = 0;
}

void wait_queue_init(struct wait_queue *wq)
{
	wq->head = wq->tail = 0;
}

static void waitq_add_locked(struct wait_queue *wq, struct process *p)
{
	p->waiting_on = wq;
	p->wait_next = 0;
	p->wait_prev = wq->tail;
	if (wq->tail)
		wq->tail->wait_next = p;
	else
		wq->head = p;
	wq->tail = p;
}

static void waitq_del_locked(struct process *p)
{
	struct wait_queue *wq = p->waiting_on;

	if (!wq)
		return;
	if (p->wait_prev)
		p->wait_prev->wait_next = p->wait_next;
	else
		wq->head = p->wait_next;
	if (p->wait_next)
		p->wait_next->wait_prev = p->wait_prev;
	else
		wq->tail = p->wait_prev;
	p->wait_next = p->wait_prev = 0;
	p->waiting_on = 0;
}

/* Current minimum vruntime among READY processes.  If nothing is ready, use
 * the running process's vruntime so a freshly woken process does not receive
 * an enormous credit relative to a CPU-bound one. */
static unsigned long long min_vruntime_locked(void)
{
	struct process *p;
	unsigned long long min = 0;
	int found = 0;

	for (p = runq_head; p; p = p->run_next) {
		if (!found || p->vruntime < min) {
			min = p->vruntime;
			found = 1;
		}
	}
	if (found)
		return min;
	p = current_proc();
	return (p && p != this_cpu()->idle) ? p->vruntime : 0;
}

static void account_locked(struct process *p)
{
	unsigned int elapsed;

	if (!p || p == this_cpu()->idle || p->state != PROC_RUNNING)
		return;
	elapsed = get_ticks_ms() - p->slice_start;
	if (elapsed) {
		p->vruntime += (unsigned long long)elapsed * p->stride;
		p->slice_start = get_ticks_ms();
	}
}

/* Clamp a woken process to the current minimum and make it runnable. */
static void wake_locked(struct process *p)
{
	struct process *cur = current_proc();
	struct cpu *c = this_cpu();

	if (!p || p->state == PROC_DEAD || p->state == PROC_UNUSED)
		return;
	if (p->waiting_on)
		waitq_del_locked(p);
	if (p->state == PROC_SLEEPING)
		sleepq_del_locked(p);
	p->state = PROC_READY;
	if (p->vruntime < min_vruntime_locked())
		p->vruntime = min_vruntime_locked();
	runq_add_locked(p);
	wakeups++;
	if (cur && (p->affinity & (1u << c->id)) &&
	    (cur == c->idle || p->weight >= cur->weight)) {
		c->need_resched = 1;
		/* The idle timer period is 10 ms; shorten it so the woken
		 * process runs at the next millisecond instead. */
		smp_resched_wake();
	}
}

void sched_add_ready(struct process *p)
{
	unsigned int flags = sched_lock_acquire();

	if (!p->weight) {
		p->weight = SCHED_WEIGHT_DEFAULT;
		p->stride = weight_stride(p->weight);
		p->vruntime = min_vruntime_locked();
	}
	p->state = PROC_READY;
	runq_add_locked(p);
	sched_lock_release(flags);
}

void sched_unlink_locked(struct process *p)
{
	runq_del_locked(p);
	if (p->waiting_on)
		waitq_del_locked(p);
	if (p->state == PROC_SLEEPING)
		sleepq_del_locked(p);
}

void sched_unlink(struct process *p)
{
	unsigned int flags = sched_lock_acquire();

	sched_unlink_locked(p);
	sched_lock_release(flags);
}

void sched_set_weight_locked(struct process *p, unsigned int weight)
{
	if (!p)
		return;
	if (weight < 1)
		weight = 1;
	if (weight > SCHED_WEIGHT_MAX)
		weight = SCHED_WEIGHT_MAX;
	if (p->state == PROC_RUNNING)
		account_locked(p);
	p->weight = weight;
	p->stride = weight_stride(weight);
}

/* Add the current process to a wait queue.  The caller holds interrupts off
 * and must restore the flags and call sched_yield() itself. */
void sched_block_on_locked(struct wait_queue *wq)
{
	struct process *p = current_proc();

	if (!p)
		return;
	runq_del_locked(p);
	waitq_add_locked(wq, p);
	p->state = PROC_BLOCKED;
	p->on_runq = 0;
}

void sched_block_on(struct wait_queue *wq)
{
	unsigned int flags;

	if (this_cpu()->preempt_count)
		return;
	flags = sched_lock_acquire();
	sched_block_on_locked(wq);
	sched_lock_release(flags);
	sched_yield();
}

/* Block on a wait queue with a deadline.  Either a waiter wake or the sleep
 * list expiry removes the process from both queues. */
void sched_block_timeout(struct wait_queue *wq, unsigned int ms)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return;
	if (ms == 0) {
		sched_block_on(wq);
		return;
	}
	if (this_cpu()->preempt_count)
		return;
	flags = sched_lock_acquire();
	p->wake_ms = get_ticks_ms() + ms;
	p->state = PROC_SLEEPING;
	runq_del_locked(p);
	waitq_add_locked(wq, p);
	sleepq_add_locked(p);
	sched_lock_release(flags);
	sched_yield();
}

static void wake_one_locked(struct wait_queue *wq)
{
	struct process *p = wq->head;

	if (!p)
		return;
	waitq_del_locked(p);
	if (p->state == PROC_BLOCKED || p->state == PROC_SLEEPING)
		wake_locked(p);
}

void wait_wake_one(struct wait_queue *wq)
{
	unsigned int flags = sched_lock_acquire();

	wake_one_locked(wq);
	sched_lock_release(flags);
}

void wait_wake_all(struct wait_queue *wq)
{
	unsigned int flags = sched_lock_acquire();

	while (wq->head)
		wake_one_locked(wq);
	sched_lock_release(flags);
}

/* ---- sleeping big kernel lock ---- */

void bkl_init(void)
{
	bkl.owner = 0;
	bkl.depth = 0;
	wait_queue_init(&bkl.wait);
}

void bkl_lock(void)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return;
	if (bkl.owner == p) {
		bkl.depth++;
		return;
	}
	for (;;) {
		flags = sched_lock_acquire();
		if (!bkl.owner) {
			bkl.owner = p;
			bkl.depth = 1;
			sched_lock_release(flags);
			return;
		}
		sched_block_on_locked(&bkl.wait);
		sched_lock_release(flags);
		sched_yield();
	}
}

void bkl_unlock(void)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return;
	flags = sched_lock_acquire();
	if (bkl.owner == p) {
		if (bkl.depth > 1) {
			bkl.depth--;
		} else {
			bkl.owner = 0;
			bkl.depth = 0;
			wake_one_locked(&bkl.wait);
		}
	}
	sched_lock_release(flags);
}

int bkl_trylock(void)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return 1;
	if (bkl.owner == p) {
		bkl.depth++;
		return 1;
	}
	flags = sched_lock_acquire();
	if (!bkl.owner) {
		bkl.owner = p;
		bkl.depth = 1;
		sched_lock_release(flags);
		return 1;
	}
	sched_lock_release(flags);
	return 0;
}

/* Drop the BKL around a voluntary switch and take it back on resume.  The
 * saved depth lives in the PCB: only the owner can be holding it, and it is
 * the current process on this CPU. */
static void bkl_yield_release(void)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return;
	flags = sched_lock_acquire();
	if (bkl.owner == p) {
		p->bkl_saved = bkl.depth;
		bkl.owner = 0;
		bkl.depth = 0;
		wake_one_locked(&bkl.wait);
	}
	/* Not the owner: leave bkl_saved alone.  A nested sleep inside a
	 * bkl_pause() section must not forget that it still has to resume the
	 * paused ownership. */
	sched_lock_release(flags);
}

static void bkl_yield_reacquire(void)
{
	struct process *p = current_proc();
	unsigned int depth;

	if (!p)
		return;
	depth = p->bkl_saved;
	if (!depth)
		return;
	p->bkl_saved = 0;
	bkl_lock();
	bkl.depth = depth;
}

void bkl_pause(void)
{
	bkl_yield_release();
}

void bkl_resume(void)
{
	bkl_yield_reacquire();
}

/* Run once on the new kernel stack: queue the process this CPU just left, so
 * its stack/page directory cannot be freed while switch.S still uses them. */
void sched_finish_switch(void)
{
	struct cpu *c = this_cpu();
	struct process *p;
	unsigned int flags;

	if (!c->deferred_unlock)
		return;
	p = c->pending_zombie;
	c->pending_zombie = 0;
	flags = c->sched_flags;
	c->deferred_unlock = 0;
	sched_lock_release(flags);
	if (p)
		process_zombie_add(p);
}

void sched_init(void)
{
	unsigned int i;

	cpu_count = 1;
	runq_head = runq_tail = 0;
	sleep_head = sleep_tail = 0;
	wakeups = 0;
	sched_lock.locked = 0;
	bkl_init();

	for (i = 0; i < MAX_CPUS; i++) {
		cpus[i].current = 0;
		cpus[i].idle = 0;
		cpus[i].id = i;
		cpus[i].lapic_id = 0;
		cpus[i].need_resched = 0;
		cpus[i].preempt_count = 0;
		cpus[i].idle_ticks = 0;
		cpus[i].switches = 0;
		cpus[i].pending_zombie = 0;
		cpus[i].deferred_unlock = 0;
		cpus[i].temp_div = 0;
		cpus[i].temp_c = -1;
		cpus[i].tick_ms = 1;
	}
}

static void sched_switch_to(struct process *next)
{
	struct cpu *c = this_cpu();
	unsigned int cr3 = paging_current_cr3();

	if (next->cr3 && next->cr3 != cr3) {
		__asm__ volatile ("movl %0, %%cr3"
				  :
				  : "r"(next->cr3)
				  : "memory");
	}
	if (c->tss && next->kstack_top)
		c->tss->esp0 = next->kstack_top;
}

/* Pick the READY process with the smallest vruntime that may run on this
 * CPU.  Ties go to the lower pid for determinism. */
static struct process *pick_locked(void)
{
	struct process *p, *best = 0;
	struct cpu *c = this_cpu();
	unsigned int mask = 1u << c->id;

	for (p = runq_head; p; p = p->run_next) {
		if (!(p->affinity & mask))
			continue;
		/* A process still marked on another CPU is a wake-up race
		 * victim: it is about to reach its own CPU's schedule().  Let
		 * that CPU deal with it instead of running it twice. */
		if (p->on_cpu != PROC_NO_CPU && p->on_cpu != c->id)
			continue;
		if (!best || p->vruntime < best->vruntime ||
		    (p->vruntime == best->vruntime && p->pid < best->pid))
			best = p;
	}
	return best;
}

static void wake_sleepers_locked(void)
{
	unsigned int now = get_ticks_ms();

	while (sleep_head && (int)(sleep_head->wake_ms - now) <= 0) {
		struct process *p = sleep_head;

		sleepq_del_locked(p);
		p->state = PROC_READY;
		p->wake_ms = 0;
		wake_locked(p);
	}
}

/*
 * CFS-lite schedule.  Called from the PIT stub (after ESP has been saved) or
 * from the int 0x81 gate, always with interrupts disabled.
 */
void schedule(void)
{
	struct cpu *c = this_cpu();
	struct process *cur = c->current;
	struct process *next;
	unsigned int flags = sched_lock_acquire();

	if (cur && cur != c->idle) {
		if (cur->state == PROC_RUNNING) {
			account_locked(cur);
			cur->state = PROC_READY;
			runq_add_locked(cur);
		} else if (cur->state == PROC_READY) {
			/* Woken between setting BLOCKED/SLEEPING and entering
			 * the gate: already on the run queue. */
		} else if (cur->state == PROC_DEAD) {
			/* Defer the zombie until switch.S has loaded the new
			 * stack: its kernel stack and page directory are still
			 * live until then. */
			c->pending_zombie = cur;
		}
	}

	next = pick_locked();
	if (!next) {
		next = c->idle;
		if (!next) {
			sched_lock_release(flags);
			for (;;)
				__asm__ volatile ("cli; hlt");
		}
	}

	if (next != cur) {
		if (cur)
			cur->on_cpu = PROC_NO_CPU;
		c->switches++;
	}
	runq_del_locked(next);
	next->state = PROC_RUNNING;
	next->on_cpu = c->id;
	next->last_cpu = c->id;
	next->slice_start = get_ticks_ms();
	c->current = next;
	c->need_resched = 0;
	/* The BSP PIT and each AP's LAPIC timer tick at 1 kHz while a process
	 * runs; while a CPU is idle they drop to 100 Hz so MWAIT can hold C2
	 * for 10 ms.  The global millisecond clock stays accurate because the
	 * PIT stub adds tick_ms (see timer_tick_advance). */
	if (c->id == 0) {
		timer_set_hz(next == c->idle ? 100 : 1000);
	} else {
		unsigned int ms = (next == c->idle) ? 10 : 1;

		c->tick_ms = ms;
		lapic_timer_set_ms(ms);
	}
	sched_switch_to(next);
	/* Keep the lock across the stack switch performed by switch.S; the
	 * old process must not become runnable on another CPU while this CPU
	 * is still using its kernel stack. */
	c->sched_flags = flags;
	c->deferred_unlock = 1;
}

void sched_yield(void)
{
	struct cpu *c = this_cpu();

	/* Blocking helpers must not be used inside a preempt-disabled section.
	 * If one is, record the request instead of switching with the count
	 * still owned by the old process. */
	if (c->preempt_count) {
		c->need_resched = 1;
		return;
	}
	/* Release the BKL before switching away and take it back after being
	 * resumed; a process that never resumes (exit) simply never reacquires
	 * it.  Blocking helpers therefore need no explicit BKL handling. */
	bkl_yield_release();
	__asm__ volatile ("int $0x81" ::: "memory");
	bkl_yield_reacquire();
}

void sched_sleep_ms(unsigned int ms)
{
	struct process *p = current_proc();
	unsigned int flags;

	if (!p)
		return;
	if (ms == 0) {
		sched_yield();
		return;
	}
	if (this_cpu()->preempt_count)
		return;   /* sleeping inside a non-preemptible section is invalid */
	flags = sched_lock_acquire();
	p->wake_ms = get_ticks_ms() + ms;
	p->state = PROC_SLEEPING;
	runq_del_locked(p);
	sleepq_add_locked(p);
	sched_lock_release(flags);
	sched_yield();
}

/* Called from the PIT after ticks has been incremented.  Returns 1 when the
 * assembly should call schedule(). */
/* Common tail of the tick path: with the scheduler lock held, decide whether
 * the assembly stub should call schedule(). */
static int tick_decide_locked(struct cpu *c, struct process *p, int ap,
			      unsigned int ms)
{
	if (p) {
		p->cpu_ticks += ms;
		if (p == c->idle)
			c->idle_ticks += ms;
	}
	/* A process inside the kernel holding the big lock must not be
	 * preempted: the lock is not preemptible, and its owner may be in the
	 * middle of a kernel data structure update.  Voluntary block points
	 * release it first. */
	if (p && bkl.owner == p && p->state == PROC_RUNNING)
		return 0;

	if (c->preempt_count) {
		if (p && p != c->idle && p->state == PROC_RUNNING &&
		    (unsigned int)(get_ticks_ms() - p->slice_start) >=
		    SCHED_SLICE_TICKS)
			c->need_resched = 1;
		return 0;
	}

	/* APs only start taking work once the BSP has finished booting. */
	if (ap && !smp_sched_enabled)
		return 0;

	if (!p)
		return 0;
	if (p == c->idle) {
		if (pick_locked()) {
			c->need_resched = 1;
			return 1;
		}
		return 0;
	}
	if (p->state != PROC_RUNNING) {
		c->need_resched = 1;
		return 1;
	}
	if ((unsigned int)(get_ticks_ms() - p->slice_start) >= SCHED_SLICE_TICKS)
		c->need_resched = 1;
	return c->need_resched ? 1 : 0;
}

/* BSP PIT: also wakes the global sleep list (the PIT is the system clock). */
int sched_tick_bsp(void)
{
	struct cpu *c = this_cpu();
	struct process *p;
	unsigned int flags = sched_lock_acquire();
	int r;

	thermal_tick();
	p = c->current;
	wake_sleepers_locked();
	r = tick_decide_locked(c, p, 0, timer_tick_ms());
	sched_lock_release(flags);
	return r;
}

/* AP local timer: no global tick counter and no sleeper wake-up (the BSP PIT
 * owns both); only this CPU's scheduling decision. */
int sched_tick_ap(void)
{
	struct cpu *c = this_cpu();
	struct process *p;
	unsigned int flags = sched_lock_acquire();
	int r;

	thermal_tick();
	p = c->current;
	r = tick_decide_locked(c, p, 1, c->tick_ms);
	if (!r && p == c->idle && c->tick_ms != 10) {
		/* Entered idle without a schedule(): stretch this CPU's LAPIC
		 * period so MWAIT can hold C2 for 10 ms. */
		c->tick_ms = 10;
		lapic_timer_set_ms(10);
	}
	sched_lock_release(flags);
	return r;
}

void preempt_disable(void)
{
	this_cpu()->preempt_count++;
}

void preempt_enable(void)
{
	struct cpu *c = this_cpu();

	if (c->preempt_count)
		c->preempt_count--;
}

int sched_snapshot(struct sys_schedinfo *out)
{
	unsigned int flags, i;
	struct process *p;

	if (!out)
		return -1;
	flags = sched_lock_acquire();
	out->ncpus = cpu_count;
	out->runnable = 0;
	for (p = runq_head; p; p = p->run_next)
		out->runnable++;
	out->sleeping = 0;
	for (p = sleep_head; p; p = p->sleep_next)
		out->sleeping++;
	out->blocked = 0;
	for (i = 0; i < PROC_MAX; i++) {
		p = procs[i];
		if (p && p->state == PROC_BLOCKED)
			out->blocked++;
	}
	out->zombies = process_zombie_count_locked();
	out->switches = 0;
	out->idle_ticks = 0;
	for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
		out->switches += cpus[i].switches;
		out->idle_ticks += cpus[i].idle_ticks;
	}
	out->wakeups = wakeups;
	out->slice_ticks = SCHED_SLICE_TICKS;
	sched_lock_release(flags);
	return 0;
}

static void sched_put_dec(unsigned int n)
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

void sched_print(void)
{
	struct sys_schedinfo s;
	unsigned int uptime, idle_pm, up_s, total_s, idle_s, idle_frac, ncpu;

	if (sched_snapshot(&s) != 0)
		return;
	uptime = get_ticks_ms();
	ncpu = s.ncpus ? s.ncpus : 1;

	/* Idle is a share of the total CPU capacity, so 4 idle CPUs at 50%
	 * each report 50%, not 200%.  Avoid 64-bit division: scale to seconds
	 * first (four CPUs overflow after ~12 days; good enough for the
	 * display). */
	up_s = uptime / 1000u;
	total_s = up_s * ncpu;
	if (total_s == 0) {
		idle_pm = (s.idle_ticks >= uptime * ncpu) ? 1000 : 0;
	} else {
		idle_s = s.idle_ticks / 1000u;
		idle_frac = s.idle_ticks % 1000u;
		idle_pm = (idle_s * 1000u) / total_s;
		idle_pm += (idle_frac * 1000u / total_s) / 1000u;
		if (idle_pm > 1000u)
			idle_pm = 1000u;
	}

	io_putstr("sched: cfs, ");
	sched_put_dec(s.ncpus);
	io_putstr(" cpu, ");
	sched_put_dec(s.slice_ticks);
	io_putstr(" ms slice\nrunq ");
	sched_put_dec(s.runnable);
	io_putstr("  sleep ");
	sched_put_dec(s.sleeping);
	io_putstr("  block ");
	sched_put_dec(s.blocked);
	io_putstr("  zombie ");
	sched_put_dec(s.zombies);
	io_putstr("\nswitches ");
	sched_put_dec(s.switches);
	io_putstr("  wakeups ");
	sched_put_dec(s.wakeups);
	io_putstr("  idle ");
	sched_put_dec(idle_pm / 10);
	io_putchar('.');
	io_putchar((char)('0' + idle_pm % 10));
	io_putstr("%\n");
}
