/* Per-process anonymous heap (SYS_MEM_ALLOC/GROW/FREE).
 *
 * Ring-3 commands only have a 2 MB image/stack window, which is not enough
 * for a full-frame image.  This service maps a single growable arena at
 * USER_HEAP_BASE into the caller's page directory.  The reaper frees it with
 * the rest of the process.
 *
 * Commands that link the ring-3 libc own this arena through libc/malloc.c
 * (malloc/free/heap_reset); the raw syscalls are for commands that do not
 * link libc.  The two must not be mixed in one process. */
#ifndef SYS_MEM_H
#define SYS_MEM_H

struct process;

int sys_mem_alloc(unsigned int bytes);
int sys_mem_grow(unsigned int bytes);
int sys_mem_free(void);
void sys_mem_release(struct process *p);

#endif
