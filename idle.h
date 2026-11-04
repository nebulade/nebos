/* CPU idle: automatic deep C-states on Nehalem/Westmere, HLT elsewhere. */
#ifndef IDLE_H
#define IDLE_H

#define IDLE_C1     0
#define IDLE_C1E    1
#define IDLE_C3     2
#define IDLE_C6     3
#define IDLE_AUTO   4
#define IDLE_C3_AP  5
#define IDLE_C6_AP  6

void idle_init(void);
void cpu_idle(void);

/* Select a C-state; 0 on success, -1 when the CPU has no MWAIT. */
int idle_set_mode(int mode);
int idle_mode(void);
int idle_mwait_supported(void);
const char *idle_mode_name(int mode);

#endif
