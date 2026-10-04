/* MTRR write-combining for MMIO framebuffer ranges. */
#ifndef MTRR_H
#define MTRR_H

/* Program one variable-range MTRR as write-combining over [base, base+size).
 * No-op if the CPU lacks MTRR, has no free variable ranges, or the range
 * cannot be covered by a single size-aligned region. */
void mtrr_set_wc(unsigned int base, unsigned int size);

#endif
