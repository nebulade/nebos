/* Timer API: milliseconds since timer_init(). */
#ifndef TIMER_H
#define TIMER_H

unsigned int get_ticks_ms(void);

/* The PIT and AP LAPIC timers run at 1 kHz while a process is running and
 * drop to 100 Hz while a CPU is idle, so C2/C3 can be held for 10 ms.  The
 * global tick counter still advances in milliseconds. */
void timer_set_hz(unsigned int hz);
unsigned int timer_tick_ms(void);

/* Called from the PIT stub: advance the millisecond clock by one tick. */
void timer_tick_advance(void);

#endif
