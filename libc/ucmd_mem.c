/* Target backend for the ring-3 heap: the SYS_MEM_* arena syscalls. */

#include "ucmd_lib.h"

void *__heap_map(unsigned int bytes)
{
	int base = u_syscall1(SYS_MEM_ALLOC, (int)bytes);

	return base < 0 ? 0 : (void *)base;
}

int __heap_grow(unsigned int total_bytes)
{
	return u_syscall1(SYS_MEM_GROW, (int)total_bytes);
}

void __heap_unmap(void)
{
	u_syscall0(SYS_MEM_FREE);
}
