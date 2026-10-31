/* MTRR write-combining for MMIO framebuffer ranges. */
#ifndef MTRR_H
#define MTRR_H

/* Program one variable-range MTRR as write-combining over [base, base+size),
 * or reuse an entry an earlier boot already left there.  Returns 1 when a WC
 * range covering the request is in place and fills wc_base and wc_size with the
 * range that entry actually covers (it is size-aligned and rounded up);
 * returns 0 if the CPU lacks MTRR, has no free variable ranges, or the range
 * cannot be covered by a single size-aligned region. */
int mtrr_set_wc(unsigned int base, unsigned int size,
		unsigned int *wc_base, unsigned int *wc_size);

#endif
