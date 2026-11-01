/* Minimal x86 spinlocks for the SMP paths. */
#ifndef SPINLOCK_H
#define SPINLOCK_H

typedef struct {
	volatile unsigned int locked;
} spinlock_t;

static inline unsigned int spin_irq_save_disable(void)
{
	unsigned int flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void spin_irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

static inline void spin_pause(void)
{
	/* REP NOP is the PAUSE encoding and is harmless on older CPUs. */
	__asm__ volatile ("rep; nop" ::: "memory");
}

static inline void spin_lock(spinlock_t *l)
{
	while (__sync_lock_test_and_set(&l->locked, 1u)) {
		while (l->locked)
			spin_pause();
	}
}

static inline void spin_unlock(spinlock_t *l)
{
	__asm__ volatile ("" ::: "memory");
	l->locked = 0;
}

/* Caller saves/restores the interrupt state around the critical section. */
static inline void spin_lock_irqsave(spinlock_t *l, unsigned int *flags)
{
	*flags = spin_irq_save_disable();
	spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned int flags)
{
	spin_unlock(l);
	spin_irq_restore(flags);
}

#endif
