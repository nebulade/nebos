/* Minimal ELF32 loader. */
#ifndef ELF_H
#define ELF_H

/* Load an ELF32 image (already in memory) into page directory `cr3`, mapping
 * its PT_LOAD segments as user pages (U/S=1, R/W=1).  Segments are assumed
 * page-aligned.  Returns the entry point virtual address, or 0 on error. */
unsigned int elf_load(unsigned int cr3, const void *image, unsigned int size);

#endif
