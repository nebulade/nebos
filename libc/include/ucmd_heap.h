/* Ring-3 libc heap extensions (see libc/malloc.c).
 *
 * heap_reset() discards every live allocation and unmaps the whole
 * SYS_MEM_* arena.  It is intended for page-scoped programs such as the
 * browser; every pointer obtained from malloc becomes invalid. */
#ifndef LIBC_UCMD_HEAP_H
#define LIBC_UCMD_HEAP_H

void heap_reset(void);

#endif
