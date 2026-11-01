/* Physical memory manager: E820 detection + page-frame allocator.
 *
 * The real-mode VESA stage (vesa_stage.S) probes INT 15h E820 and stores the
 * result at E820_MAP_ADDR (0x1000, below the kernel, so it survives kexec).
 * mm_init() parses that map and builds a bitmap over physical RAM, reserving
 * the low 1 MB (kernel, DMA, UHCI, UMA, boot structures) plus the VGA shadow
 * buffer region.  Everything above that is handed out 4 KB pages at a time.
 */
#ifndef MM_H
#define MM_H

#define PAGE_SIZE        0x1000

/* E820 map storage (written by vesa_stage.S in real mode). */
#define E820_MAP_ADDR    0x1000
#define E820_MAGIC       0xE820E820
#define E820_MAX_ENTRIES 32

/* Frame allocator covers physical RAM [0, MM_MAX_PHYS).  Bitmap: 1 bit/page.
 * 128 MB matches this machine and QEMU's default; raise it (and move the
 * kernel upper bound, see kernel.ld) if larger machines are targeted. */
#define MM_MAX_PHYS      (128u * 1024 * 1024)  /* 128 MB ceiling */

/* Regions the kernel already owns (marked used at init): */
#define MM_RESERVE_LOW     0x100000   /* 0 .. 1 MB: kernel/DMA/UHCI/UMA/boot */
#define MM_SHADOW_BASE     0x100000   /* VGA shadow buffer base */
#define MM_SHADOW_RESERVE  0x800000   /* conservative 8 MB for the shadow */

/* E820 entry type codes. */
#define E820_RAM       1
#define E820_RESERVED  2

/* One E820 entry (ACPI 3.0, 24 bytes). */
struct e820_entry {
	unsigned long long base;
	unsigned long long length;
	unsigned int type;
	unsigned int attrs;
} __attribute__((packed));

struct e820_map {
	unsigned int magic;
	unsigned int count;
	struct e820_entry entry[E820_MAX_ENTRIES];
};

void mm_init(void);

/* Return the base address of a free 4 KB page, or 0 if none. */
unsigned int mm_alloc_page(void);
void mm_free_page(unsigned int addr);

/* Free a page-aligned boot-time reservation (never low 1 MB / the shadow). */
void mm_free_range(unsigned int base, unsigned int size);

/* Return n contiguous pages (base address), or 0 if unavailable. */
unsigned int mm_alloc_pages(unsigned int n);

unsigned int mm_free_pages(void);

/* Total usable RAM reported by E820, in KB. */
unsigned int mm_total_ram_kb(void);

/* E820 map accessors (count = 0 when the map is unavailable). */
unsigned int mm_e820_count(void);
const struct e820_entry *mm_e820_entry(unsigned int i);

/* Dump the memory map + allocator stats to COM1. */
void mm_dump(void);

#endif
