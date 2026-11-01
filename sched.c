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
 * The run queue is a single global list for now.  That is also the shape an
 * SMP version wants: one queue protected by a lock, with per-CPU current.
 */

#include "sched.h"
#include "io.h"
#include "paging.h"
#include "process.h"
#include "syscall.h"
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

/* switch.S reads these two facts directly: current at %gs:0 and esp at
 * offset 0 of the process.  Keep them true. */
_Static_assert(__builtin_offsetof(struct cpu, current) == 0,
	       "switch.S reads current from %gs:0");
_Static_assert(__builtin_offsetof(struct process, esp) == 0,
	       "switch.S saves the interrupted esp at offset 0");

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
	if (cur && (cur == c->idle ||
		    p->weight >= cur->weight))
		c->need_resched = 1;
}

void sched_add_ready(struct process *p)
{
	if (!p->weight) {
		p->weight = SCHED_WEIGHT_DEFAULT;
		p->stride = weight_stride(p->weight);
		p->vruntime = min_vruntime_locked();
	}
	p->state = PROC_READY;
	runq_add_locked(p);
}

void sched_unlink(struct process *p)
{
	runq_del_locked(p);
	if (p->waiting_on)
		waitq_del_locked(p);
	if (p->state == PROC_SLEEPING)
		sleepq_del_locked(p);
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
	flags = irq_save_disable();
	sched_block_on_locked(wq);
	irq_restore(flags);
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
	flags = irq_save_disable();
	p->wake_ms = get_ticks_ms() + ms;
	p->state = PROC_SLEEPING;
	runq_del_locked(p);
	waitq_add_locked(wq, p);
	sleepq_add_locked(p);
	irq_restore(flags);
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
	unsigned int flags = irq_save_disable();

	wake_one_locked(wq);
	irq_restore(flags);
}

void wait_wake_all(struct wait_queue *wq)
{
	unsigned int flags = irq_save_disable();

	while (wq->head)
		wake_one_locked(wq);
	irq_restore(flags);
}

void sched_init(void)
{
	unsigned int i;

	cpu_count = 1;
	runq_head = runq_tail = 0;
	sleep_head = sleep_tail = 0;
	wakeups = 0;

	for (i = 0; i < MAX_CPUS; i++) {
		cpus[i].current = 0;
		cpus[i].idle = 0;
		cpus[i].id = i;
		cpus[i].lapic_id = 0;
		cpus[i].need_resched = 0;
		cpus[i].preempt_count = 0;
		cpus[i].idle_ticks = 0;
		cpus[i].switches = 0;
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
	unsigned int mask = 1u << this_cpu()->id;

	for (p = runq_head; p; p = p->run_next) {
		if (!(p->affinity & mask))
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

	if (cur && cur != c->idle) {
		if (cur->state == PROC_RUNNING) {
			account_locked(cur);
			cur->state = PROC_READY;
			runq_add_locked(cur);
		} else if (cur->state == PROC_READY) {
			/* Woken between setting BLOCKED/SLEEPING and entering
			 * the gate: already on the run queue. */
		} else if (cur->state == PROC_DEAD) {
			/* Keep the PCB alive until the kworker can free it, but
			 * unpublish it immediately. */
			process_zombie_add(cur);
		}
	}

	next = pick_locked();
	if (!next) {
		next = c->idle;
		if (!next) {
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
	sched_switch_to(next);
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
	__asm__ volatile ("int $0x81" ::: "memory");
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
	flags = irq_save_disable();
	p->wake_ms = get_ticks_ms() + ms;
	p->state = PROC_SLEEPING;
	runq_del_locked(p);
	sleepq_add_locked(p);
	irq_restore(flags);
	sched_yield();
}

/* Called from the PIT after ticks has been incremented.  Returns 1 when the
 * assembly should call schedule(). */
int sched_tick_bsp(void)
{
	struct cpu *c = this_cpu();
	struct process *p = c->current;

	if (p) {
		p->cpu_ticks++;
		if (p == c->idle)
			c->idle_ticks++;
	}

	wake_sleepers_locked();

	if (c->preempt_count) {
		if (p && p != c->idle && p->state == PROC_RUNNING &&
		    (unsigned int)(get_ticks_ms() - p->slice_start) >=
		    SCHED_SLICE_TICKS)
			c->need_resched = 1;
		return 0;
	}

	if (!p)
		return 0;
	if (p == c->idle) {
		if (runq_head) {
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
	flags = irq_save_disable();
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
	out->zombies = process_zombie_count();
	out->switches = cpus[0].switches;
	out->wakeups = wakeups;
	out->idle_ticks = cpus[0].idle_ticks;
	out->slice_ticks = SCHED_SLICE_TICKS;
	irq_restore(flags);
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
	unsigned int uptime, idle_pm, up_s, idle_s, idle_frac;

	if (sched_snapshot(&s) != 0)
		return;
	uptime = get_ticks_ms();

	/* Avoid 64-bit division: scale to seconds first.  ticks wraps at ~49
	 * days, so idle_s * 1000 stays within 32 bits for the useful range. */
	up_s = uptime / 1000u;
	if (up_s == 0) {
		idle_pm = (s.idle_ticks >= uptime) ? 1000 : 0;
	} else {
		idle_s = s.idle_ticks / 1000u;
		idle_frac = s.idle_ticks % 1000u;
		idle_pm = (idle_s * 1000u) / up_s;
		idle_pm += (idle_frac * 1000u / up_s) / 1000u;
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
