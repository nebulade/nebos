/* CPU idle: deep C-state support via mwait when available. */
#ifndef IDLE_H
#define IDLE_H

void idle_init(void);
void cpu_idle(void);

#endif
