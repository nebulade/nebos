/* Minimal ELF32 loader: parse PT_LOAD segments, allocate physical pages,
 * copy file bytes, zero BSS, and map them into a page directory as user pages. */

#include "elf.h"
#include "mm.h"
#include "paging.h"

#define ELF_MAGIC 0x464C457F   /* "\x7fELF" little-endian */
#define PT_LOAD   1
#define EM_386    3

struct elf32_hdr {
	unsigned char ident[16];
	unsigned short type;
	unsigned short machine;
	unsigned int   version;
	unsigned int   entry;
	unsigned int   phoff;
	unsigned int   shoff;
	unsigned int   flags;
	unsigned short ehsize;
	unsigned short phentsize;
	unsigned short phnum;
	unsigned short shentsize;
	unsigned short shnum;
	unsigned short shstrndx;
};

struct elf32_phdr {
	unsigned int type;
	unsigned int offset;
	unsigned int vaddr;
	unsigned int paddr;
	unsigned int filesz;
	unsigned int memsz;
	unsigned int flags;
	unsigned int align;
};

static void memzero(void *dst, unsigned int n)
{
	unsigned char *p = (unsigned char *)dst;
	while (n--)
		*p++ = 0;
}

static void memcopy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--)
		*d++ = *s++;
}

unsigned int elf_load(unsigned int cr3, const void *image, unsigned int size)
{
	const struct elf32_hdr *h = (const struct elf32_hdr *)image;
	const struct elf32_phdr *ph;
	unsigned int i, p;

	if (size < sizeof(*h))
		return 0;
	if (*(const unsigned int *)h->ident != ELF_MAGIC)
		return 0;
	if (h->machine != EM_386)
		return 0;

	ph = (const struct elf32_phdr *)((const char *)image + h->phoff);

	for (i = 0; i < h->phnum; i++) {
		unsigned int va, memsz, filesz, off, npages;

		if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
			continue;

		va = ph[i].vaddr;
		memsz = ph[i].memsz;
		filesz = ph[i].filesz;
		off = ph[i].offset;
		npages = (memsz + 0xFFF) >> 12;

		for (p = 0; p < npages; p++) {
			unsigned int phys = mm_alloc_page();
			unsigned int src = p * 0x1000;
			unsigned int n;

			if (!phys)
				return 0;
			memzero((void *)phys, 0x1000);
			if (src < filesz) {
				n = filesz - src;
				if (n > 0x1000)
					n = 0x1000;
				memcopy((void *)phys,
					(const char *)image + off + src, n);
			}
			if (!paging_map_page(cr3, va + src, phys,
					     PTE_USER | PTE_WRITABLE | PTE_PRESENT))
				return 0;
		}
	}

	/* Flush the caches so the freshly-copied code is coherent.  The Pentium
	 * (P5) has split I/D caches with no hardware coherency for self-modifying
	 * code, so the code we just wrote through the data cache is not visible to
	 * the instruction cache until we wbinvd.  QEMU doesn't model this. */
	__asm__ volatile ("wbinvd");

	return h->entry;
}
