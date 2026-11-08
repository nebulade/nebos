/* Per-process anonymous heap (SYS_MEM_ALLOC/GROW/FREE).
 *
 * Ring-3 commands only have a 2 MB image/stack window, which is not enough
 * for a full-frame image.  This service maps a single growable arena at
 * USER_HEAP_BASE into the caller's page directory.  The reaper frees it with
 * the rest of the process.
 *
 * Ring-3 commands use the libc heap (libc/malloc.c: malloc/free/heap_reset),
 * which owns this arena.  The SYS_MEM_* syscalls are the libc backend
 * (libc/ucmd_mem.c) and are not called directly by application code. */
#ifndef SYS_MEM_H
#define SYS_MEM_H

struct process;

int sys_mem_alloc(unsigned int bytes);
int sys_mem_grow(unsigned int bytes);
int sys_mem_free(void);
void sys_mem_release(struct process *p);

#endif
