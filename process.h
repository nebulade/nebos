/* Preemptive round-robin multitasking for i586, ring-0 only. */
#ifndef PROCESS_H
#define PROCESS_H

#define PROC_STACK_SIZE 0x1000  /* 4 KB per process (= 1 page) */

/* Process stacks are allocated from the frame allocator (mm.c), i.e. above the
 * VGA shadow buffer.  If that is unavailable (no E820 map) we fall back to a
 * small fixed pool below 1 MB so the system still boots with fewer processes. */
#define PROC_FALLBACK_STACK_BASE 0x42000
#define PROC_FALLBACK_STACK_MAX  8

#define PROC_UNUSED  0
#define PROC_READY   1
#define PROC_RUNNING 2
#define PROC_DEAD    3

#define TIMESLICE_TICKS 20 /* context switch every 20 ms (PIT @ 1 kHz) */

#define PROC_NAME_LEN 12

struct text_screen;  /* forward decl (see screen.h) */

struct process {
	unsigned int esp;
	unsigned int state;
	unsigned int pid;
	char         name[PROC_NAME_LEN];
	struct text_screen *screen;  /* output target for io_putchar & co. */
	unsigned int stack_base;     /* page to free on exit, or 0 (bootstrap/fallback) */
};

/* Dynamically-sized process table (grown on demand; see process.c). */
extern struct process *procs;
extern volatile unsigned int current_pid;

void process_init(void);
int  process_create(const char *name, void (*entry)(void));
int  process_create_arg(const char *name, void (*entry)(const char *),
			const char *arg);
int  process_create_ptr(const char *name, void (*entry)(void *), void *arg);
void process_set_screen(struct text_screen *screen);
void process_exit(void);
void schedule(void);
void process_ps(void);
int  process_kill(unsigned int pid);
int  process_is_alive(unsigned int pid);
void process_wait(unsigned int pid);

#endif
