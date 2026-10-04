/* Kernel heap allocator (kmalloc/kfree) on top of the page-frame allocator.
 *
 * Power-of-two segregated free lists:
 *   - small allocations (<= one page) come from size-class free lists, which
 *     are refilled by carving whole pages from the frame allocator.
 *   - large allocations (> one page) allocate contiguous pages directly.
 *
 * Each block carries a 4-byte header immediately before the returned pointer;
 * it stores the block size (so kfree knows which free list / how many pages).
 */
#ifndef KMALLOC_H
#define KMALLOC_H

void *kmalloc(unsigned int size);
void kfree(void *ptr);
void kmalloc_init(void);

#endif
