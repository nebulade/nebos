/* Host backend for the malloc test: a fixed 32 MB, page-aligned arena.
 *
 * New pages are poisoned instead of zeroed, which catches an allocator that
 * accidentally relies on the kernel's zeroed initial mapping.
 */

#include <stddef.h>
#include <string.h>

#define HOST_ARENA_BYTES (32u * 1024u * 1024u)

static unsigned char arena[HOST_ARENA_BYTES] __attribute__((aligned(4096)));
static unsigned int mapped;

void *__heap_map(unsigned int bytes)
{
	if (bytes == 0 || bytes > HOST_ARENA_BYTES || mapped)
		return 0;
	mapped = bytes;
	memset(arena, 0xA5, bytes);
	return arena;
}

int __heap_grow(unsigned int total_bytes)
{
	if (total_bytes > HOST_ARENA_BYTES || total_bytes < mapped)
		return -1;
	memset(arena + mapped, 0xA5, total_bytes - mapped);
	mapped = total_bytes;
	return 0;
}

void __heap_unmap(void)
{
	mapped = 0;
}

unsigned int host_mem_mapped(void)
{
	return mapped;
}
