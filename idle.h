/* CPU idle: deep C-state support via mwait when available. */
#ifndef IDLE_H
#define IDLE_H

void idle_init(void);
void idle_set_cstate(unsigned int level);
unsigned int idle_get_cstate(void);
int idle_has_mwait(void);
void cpu_idle(void);

#endif
