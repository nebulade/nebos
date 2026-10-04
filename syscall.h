/* Syscall interface (int 0x80). */
#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_PUTSTR       1
#define SYS_GET_TICKS_MS 2
#define SYS_SLEEP_MS     3
#define SYS_EXIT         4

int syscall_dispatch(int num, int a1, int a2, int a3);

#endif
