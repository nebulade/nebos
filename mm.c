/* Physical memory manager: parse the E820 map and hand out 4 KB pages. */

#include "mm.h"
#include "bootinfo.h"
#include "serial.h"
#include "vga.h"

/* One bit per 4 KB page; bit set = in use. */
#define MM_BITMAP_BITS  (MM_MAX_PHYS / PAGE_SIZE)
#define MM_BITMAP_WORDS (MM_BITMAP_BITS / 32)

static unsigned int bitmap[MM_BITMAP_WORDS];
static unsigned int free_pages;
static unsigned int total_ram_kb;

/* ---- bitmap helpers ---- */

static inline void bit_set(unsigned int idx)
{
	bitmap[idx / 32] |= (1u << (idx % 32));
}

static inline void bit_clear(unsigned int idx)
{
	bitmap[idx / 32] &= ~(1u << (idx % 32));
}

static inline int bit_test(unsigned int idx)
{
	return (bitmap[idx / 32] >> (idx % 32)) & 1;
}

/* ---- interrupt flag save/restore (allocator is not IRQ-safe) ---- */

static inline unsigned int irq_disable_save(void)
{
	unsigned int flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" : : "r"(flags));
}

/* Mark a physical byte range [base, base+len) as used in the bitmap. */
static void mark_used(unsigned long long base, unsigned long long len)
{
	unsigned long long end = base + len;
	unsigned int p0, p1, p;

	if (base >= MM_MAX_PHYS)
		return;
	if (end > MM_MAX_PHYS)
		end = MM_MAX_PHYS;

	p0 = (unsigned int)(base >> 12);
	p1 = (unsigned int)((end + PAGE_SIZE - 1) >> 12);
	for (p = p0; p < p1; p++)
		bit_set(p);
}

/* Mark a physical byte range [base, base+len) as free (usable RAM). */
static void mark_free(unsigned long long base, unsigned long long len)
{
	unsigned long long end = base + len;
	unsigned int p0, p1, p;

	if (base >= MM_MAX_PHYS)
		return;
	if (end > MM_MAX_PHYS)
		end = MM_MAX_PHYS;

	p0 = (unsigned int)(base >> 12);
	p1 = (unsigned int)(end >> 12);
	for (p = p0; p < p1; p++)
		bit_clear(p);
}

/* Mark the whole bitmap as used, then walk the E820 map and free RAM. */
void mm_init(void)
{
	const struct e820_map *map = (const struct e820_map *)E820_MAP_ADDR;
	unsigned int i, n;

	/* Everything starts in-use. */
	for (i = 0; i < MM_BITMAP_WORDS; i++)
		bitmap[i] = 0xFFFFFFFFu;

	total_ram_kb = 0;

	if (map->magic == E820_MAGIC && map->count <= E820_MAX_ENTRIES) {
		n = map->count;
		for (i = 0; i < n; i++) {
			const struct e820_entry *e = &map->entry[i];
			if (e->type == E820_RAM && e->length != 0) {
				unsigned long long end = e->base + e->length;

				mark_free(e->base, e->length);
				if (end > MM_MAX_PHYS)
					end = MM_MAX_PHYS;
				if (end > e->base)
					total_ram_kb +=
						(unsigned int)((end - e->base) >> 10);
			}
		}
	} else {
		/* No E820 map (very old BIOS, or first kexec onto an old build):
		 * assume 16 MB of RAM from 1 MB up.  The process-stack fallback
		 * pool keeps the system bootable if this guess is wrong. */
		mark_free(0x100000ULL, 0x1000000ULL - 0x100000ULL);
		total_ram_kb = (0x1000000ULL - 0x100000ULL) >> 10; /* 15 MB */
	}

	/* Re-reserve what the kernel already owns. */
	mark_used(0, MM_RESERVE_LOW);
	mark_used(MM_SHADOW_BASE, MM_SHADOW_RESERVE);

	/* The VBE LFB is normally high MMIO, but a BIOS may place it inside
	 * low RAM.  Never hand framebuffer pages to the allocator: the boot
	 * console draws into the LFB, so an allocated page directory there
	 * would be overwritten by its own progress line. */
	{
		const volatile struct vga_boot_fb_info *fb = VGA_BOOT_FB;
		unsigned int sz;

		__asm__ volatile ("" : "+r"(fb));
		sz = (unsigned int)fb->pitch * fb->height;
		if (!sz)
			sz = 0x1000000;
		if (fb->valid && fb->lfb_base && fb->lfb_base < MM_MAX_PHYS)
			mark_used(fb->lfb_base, sz);
	}

	/* The boot path copied the command-pack rootfs into high RAM before
	 * the kernel ran.  Keep it away from the allocator until ramfs_init()
	 * has taken its own copy. */
	{
		unsigned int pa = bootinfo_pack_addr();
		unsigned int ps = bootinfo_pack_size();

		if (pa >= MM_RESERVE_LOW && pa < MM_MAX_PHYS && ps != 0 &&
		    ps <= MM_MAX_PHYS && pa + ps > pa &&
		    pa + ps <= MM_MAX_PHYS)
			mark_used(pa, ps);
	}

	/* Count free pages. */
	free_pages = 0;
	for (i = 0; i < MM_BITMAP_BITS; i++) {
		if (!bit_test(i))
			free_pages++;
	}
}

unsigned int mm_alloc_page(void)
{
	unsigned int i, w;
	unsigned int flags = irq_disable_save();

	for (w = 0; w < MM_BITMAP_WORDS; w++) {
		if (bitmap[w] != 0xFFFFFFFFu) {
			unsigned int word = bitmap[w];
			for (i = 0; i < 32; i++) {
				if (!(word & (1u << i))) {
					unsigned int idx = w * 32 + i;
					bitmap[w] |= (1u << i);
					free_pages--;
					irq_restore(flags);
					return idx * PAGE_SIZE;
				}
			}
		}
	}

	irq_restore(flags);
	return 0;
}

unsigned int mm_alloc_pages(unsigned int n)
{
	unsigned int start = 0, run = 0;
	unsigned int p;
	unsigned int flags;

	if (n == 0)
		return 0;

	flags = irq_disable_save();
	for (p = 0; p < MM_BITMAP_BITS; p++) {
		if (!bit_test(p)) {
			if (run == 0)
				start = p;
			run++;
			if (run == n) {
				unsigned int i;
				for (i = start; i < start + n; i++)
					bit_set(i);
				free_pages -= n;
				irq_restore(flags);
				return start * PAGE_SIZE;
			}
		} else {
			run = 0;
		}
	}
	irq_restore(flags);
	return 0;
}

void mm_free_page(unsigned int addr)
{
	unsigned int idx = addr / PAGE_SIZE;
	unsigned int flags;

	if (addr == 0 || idx >= MM_BITMAP_BITS)
		return;

	flags = irq_disable_save();
	if (bit_test(idx)) {
		bit_clear(idx);
		free_pages++;
	}
	irq_restore(flags);
}

/* Return a boot-time reservation (the rootfs pack region) to the allocator.
 * Low memory and the VGA shadow reserve always stay reserved. */
void mm_free_range(unsigned int base, unsigned int size)
{
	unsigned int end, p;

	if (base == 0 || size == 0)
		return;
	base &= ~(PAGE_SIZE - 1u);
	if (base + size < base)
		return;
	end = (base + size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (base < MM_SHADOW_BASE + MM_SHADOW_RESERVE)
		return;
	for (p = base; p < end; p += PAGE_SIZE)
		mm_free_page(p);
}

unsigned int mm_free_pages(void)
{
	return free_pages;
}

unsigned int mm_total_ram_kb(void)
{
	return total_ram_kb;
}

unsigned int mm_e820_count(void)
{
	const struct e820_map *map = (const struct e820_map *)E820_MAP_ADDR;
	if (map->magic == E820_MAGIC && map->count <= E820_MAX_ENTRIES)
		return map->count;
	return 0;
}

const struct e820_entry *mm_e820_entry(unsigned int i)
{
	const struct e820_map *map = (const struct e820_map *)E820_MAP_ADDR;
	if (map->magic == E820_MAGIC && map->count <= E820_MAX_ENTRIES &&
	    i < map->count)
		return &map->entry[i];
	return 0;
}

void mm_dump(void)
{
	const struct e820_map *map = (const struct e820_map *)E820_MAP_ADDR;
	unsigned int i;

	serial_puts(SERIAL_COM1, "mm: E820 ");
	if (map->magic == E820_MAGIC && map->count <= E820_MAX_ENTRIES) {
		serial_putdec(SERIAL_COM1, map->count);
		serial_puts(SERIAL_COM1, " entries, ");
		for (i = 0; i < map->count; i++) {
			serial_puts(SERIAL_COM1, "\n  [");
			serial_puthex(SERIAL_COM1, (unsigned int)map->entry[i].base);
			serial_puts(SERIAL_COM1, " +");
			serial_puthex(SERIAL_COM1, (unsigned int)map->entry[i].length);
			serial_puts(SERIAL_COM1, "] type ");
			serial_putdec(SERIAL_COM1, map->entry[i].type);
		}
	} else {
		serial_puts(SERIAL_COM1, "unavailable (fallback 16 MB)");
	}
	serial_puts(SERIAL_COM1, "\nmm: total ");
	serial_putdec(SERIAL_COM1, total_ram_kb);
	serial_puts(SERIAL_COM1, " KB, free ");
	serial_putdec(SERIAL_COM1, free_pages * (PAGE_SIZE / 1024));
	serial_puts(SERIAL_COM1, " KB\n");
}
