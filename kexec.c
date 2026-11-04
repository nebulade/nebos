/* kexec: receive a new kernel + rootfs pack over serial and jump into it.
 *
 * C-light reload: we do NOT return to real mode or re-run the VESA stage.
 * Instead we reuse the current framebuffer (the boot FB info at 0x600 and the
 * LFB are still valid) and jump straight to the new kernel's kernel_entry,
 * which re-inits IDT/PIC/PIT/mouse/process/VGA-shadow/terminal from scratch.
 *
 * Because this code lives inside the kernel region we overwrite, the final
 * "copy image to 0x7E00 and jump" is done by a small position-independent
 * trampoline relocated to 0x800 (below the kernel).
 *
 * The terminal polls kexec_pending() on its idle loop, so `make dev` on the
 * host alone triggers the reload (no typing required). The manual `kexec`
 * command is kept as a fallback that waits for the same image.
 *
 * Wire protocol (host -> us):
 *   4 bytes: magic "KXE2" (LE)    -- new magic: old kernels ignore it
 *   4 bytes: entry address (LE)   -- new kernel_entry VMA
 *   4 bytes: kernel_size (LE)     -- kernel.bin byte count
 *   4 bytes: pack_size (LE)       -- rootfs gzip byte count (0 = keep running)
 *   N bytes: kernel.bin data
 *   M bytes: rootfs pack data
 *   1 byte:  checksum (sum of both payloads mod 256)
 */

#include "kexec.h"
#include "bootinfo.h"
#include "io.h"
#include "mm.h"
#include "net.h"
#include "netdev.h"
#include "ramfs.h"
#include "serial.h"
#include "smp.h"

#define KEXEC_MAGIC 0x3245584B /* "KXE2" little-endian */

/* Staging buffer for the received image: allocated from the frame allocator
 * when the header arrives, so no fixed low-memory region is needed.  It sits
 * above the VGA shadow reserve (>= 0x900000), far from the 0x7E00 load
 * address, so the trampoline's forward copy can never overlap itself. */
unsigned int kexec_stage;
static unsigned int kexec_stage_pages;

int kexec_stage_alloc(unsigned int size)
{
	unsigned int pages;

	if (size == 0 || size > KEXEC_MAX)
		return 0;
	pages = (size + 0xFFF) >> 12;
	kexec_stage = mm_alloc_pages(pages);
	if (!kexec_stage)
		return 0;
	kexec_stage_pages = pages;
	return 1;
}

void kexec_stage_free(void)
{
	unsigned int i;

	for (i = 0; i < kexec_stage_pages; i++)
		mm_free_page(kexec_stage + i * 0x1000);
	kexec_stage_pages = 0;
	kexec_stage = 0;
}

#define KEXEC_TRAMP 0x800      /* relocated trampoline (below the kernel) */

volatile int kexec_busy;
volatile unsigned int kexec_progress;
volatile unsigned int kexec_total;

extern unsigned char kexec_trampoline[];
extern unsigned char kexec_trampoline_end[];

int kexec_pending(void)
{
	unsigned char b, x, e, c;

	/* Scan for the "KXE2" magic, draining any leading bytes that can't begin
	 * it. This self-heals after a stray/partial/overflowed transfer, so stale
	 * bytes in the ring buffer can never permanently block detection. */
	for (;;) {
		if (!serial_ring_peek(0, &b))
			return 0;                 /* buffer empty */
		if (b != 'K') {
			(void)serial_ring_getc(); /* stale byte, drop it */
			continue;
		}
		/* First byte is 'K'. Report pending only once all 4 bytes are here. */
		if (serial_ring_peek(1, &x) && serial_ring_peek(2, &e) &&
		    serial_ring_peek(3, &c)) {
			if (x == 'X' && e == 'E' && c == '2')
				return 1;         /* "KXE2" found */
			(void)serial_ring_getc(); /* false 'K' start, drop it */
			continue;
		}
		return 0;                     /* 'K' seen but magic incomplete; wait */
	}
}

/* Drain whatever partial data remains in the ring buffer after a failure so a
 * subsequent attempt starts clean. */
static void kexec_drain(void)
{
	while (serial_has_data(SERIAL_COM1))
		(void)serial_ring_getc();
}

static int kexec_fail(const char *msg)
{
	if (msg)
		io_putstr(msg);
	kexec_drain();
	return -1;
}

/* Publish the boot-info block the new kernel reads.  The pack source is the
 * staging tail for a full reload, or the running ramfs for a kernel-only one;
 * ramfs_init() takes its own copy and releases the region. */
static unsigned int pack_ip(const unsigned char ip[4])
{
	return (unsigned int)ip[0] | ((unsigned int)ip[1] << 8) |
	       ((unsigned int)ip[2] << 16) | ((unsigned int)ip[3] << 24);
}

static void kexec_write_bootinfo(unsigned int pack_addr, unsigned int pack_size)
{
	volatile struct boot_info *bi =
		(volatile struct boot_info *)BOOT_INFO_ADDR;
	unsigned int words[BOOT_INFO_WORDS];
	unsigned int rsdp = 0, flags = 0, sum = 0, i;
	unsigned char ip[4], mask[4], gw[4], dns[4];

	__asm__ volatile ("" : "+r"(bi));

	if (bootinfo_valid()) {
		rsdp = bootinfo_rsdp();
		flags = bootinfo_flags();
	}
	for (i = 0; i < BOOT_INFO_WORDS; i++)
		words[i] = 0;
	words[0] = BOOT_INFO_MAGIC;
	words[1] = BOOT_INFO_VERSION;
	words[3] = rsdp;
	words[4] = flags;
	words[5] = pack_addr;
	words[6] = pack_size;
	/* Keep the running IP configuration across the jump: the new kernel's
	 * boot DHCP only has a four-second link/DHCP window, and a net-dev
	 * reload otherwise comes back on the static fallback address. */
	net_get_ip(ip);
	if (ip[0] || ip[1] || ip[2] || ip[3]) {
		net_get_mask(mask);
		net_get_gateway(gw);
		net_get_dns(dns);
		flags |= BOOTINFO_FLAG_NET;
		words[7] = pack_ip(ip);
		words[8] = pack_ip(mask);
		words[9] = pack_ip(gw);
		words[10] = pack_ip(dns);
	}
	for (i = 0; i < BOOT_INFO_WORDS; i++)
		sum += words[i];
	words[2] = 0u - sum;

	bi->magic = words[0];
	bi->version = words[1];
	bi->checksum = words[2];
	bi->rsdp = words[3];
	bi->flags = words[4];
	bi->pack_addr = words[5];
	bi->pack_size = words[6];
	for (i = 0; i < 5; i++)
		bi->reserved[i] = words[7 + i];
}

static int kexec_recv(unsigned int *entry, unsigned int *kernel_size,
		      unsigned int *pack_size)
{
	unsigned int i, magic = 0, n, total;
	unsigned char c, sum = 0, chk;
	volatile unsigned char *dst;

	for (i = 0; i < 4; i++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (magic)\n");
		magic |= (unsigned int)c << (8 * i);
	}
	if (magic != KEXEC_MAGIC)
		return kexec_fail("kexec: bad magic\n");

	*entry = 0;
	for (i = 0; i < 4; i++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (entry)\n");
		*entry |= (unsigned int)c << (8 * i);
	}

	*kernel_size = 0;
	for (i = 0; i < 4; i++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (kernel size)\n");
		*kernel_size |= (unsigned int)c << (8 * i);
	}
	*pack_size = 0;
	for (i = 0; i < 4; i++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (pack size)\n");
		*pack_size |= (unsigned int)c << (8 * i);
	}

	if (*kernel_size == 0 || *kernel_size > KEXEC_MAX ||
	    *pack_size > KEXEC_MAX ||
	    *kernel_size + *pack_size < *kernel_size ||
	    *kernel_size + *pack_size > KEXEC_MAX)
		return kexec_fail("kexec: bad size\n");
	total = *kernel_size + *pack_size;
	if (!kexec_stage_alloc(total))
		return kexec_fail("kexec: out of memory (staging)\n");
	dst = (volatile unsigned char *)kexec_stage;

	kexec_total = total;
	kexec_progress = 0;

	for (n = 0; n < total; n++) {
		if (!serial_ring_wait(5000, &c)) {
			kexec_stage_free();
			return kexec_fail("kexec: timeout (data)\n");
		}
		dst[n] = c;
		sum += c;
		kexec_progress = n + 1;
		if ((n & 0xFFF) == 0xFFF)
			io_putchar('.');
	}
	io_putchar('\n');

	if (!serial_ring_wait(5000, &chk)) {
		kexec_stage_free();
		return kexec_fail("kexec: timeout (checksum)\n");
	}
	if (chk != sum) {
		kexec_stage_free();
		return kexec_fail("kexec: checksum mismatch\n");
	}

	return 0;
}

void program_kexec_main(void)
{
	unsigned int entry = 0, kernel_size = 0, pack_size = 0;

	io_putstr("kexec: waiting for kernel/rootfs over serial...\n");
	if (kexec_recv(&entry, &kernel_size, &pack_size) != 0) {
		io_putstr("kexec: aborted\n");
		kexec_busy = 0;
		return;
	}

	io_putstr("kexec: received kernel ");
	io_puthex(kernel_size);
	io_putstr(" bytes, rootfs ");
	io_puthex(pack_size);
	io_putstr(" bytes, entry ");
	io_puthex(entry);
	io_putstr("\n");

	if (pack_size)
		kexec_jump(entry, kernel_size, kexec_stage + kernel_size,
			   pack_size);
	else
		kexec_jump(entry, kernel_size, 0, 0);
}

/* Relocate the trampoline to 0x800 (it survives the kernel overwrite) and
 * jump to it with the staging/load/size/entry arguments.  Never returns.
 * A zero pack_size keeps the running rootfs. */
void kexec_jump(unsigned int entry, unsigned int kernel_size,
		unsigned int pack_addr, unsigned int pack_size)
{
	if (pack_size == 0) {
		pack_addr = (unsigned int)ramfs_pack_base();
		pack_size = ramfs_pack_size();
	}

	/* Stop NIC DMA before overwriting the kernel: otherwise the old
	 * receive ring keeps writing into pages the new kernel reuses. */
	netdev_shutdown();

	/* Park the APs first: the new kernel image is about to overwrite the
	 * kernel they are executing.  INIT puts them back in real mode where
	 * they wait for the new kernel's SIPI. */
	smp_stop_aps();
	kexec_write_bootinfo(pack_addr, pack_size);

	/* Relocate the trampoline to 0x800 so it survives the kernel overwrite. */
	{
		const void *s = (const void *)kexec_trampoline;
		void *d = (void *)KEXEC_TRAMP;
		unsigned int n = (unsigned int)(kexec_trampoline_end - kexec_trampoline);
		__asm__ volatile (
			"cld\n\t"
			"rep movsb"
			: "+S"(s), "+D"(d), "+c"(n)
			:
			: "memory");
	}

	/* Serialize so the freshly-written trampoline is visible to fetch. */
	__asm__ volatile ("cpuid" ::: "eax", "ebx", "ecx", "edx");

	/* Jump to the trampoline with args in eax/ebx/ecx/edx.  Only the kernel
	 * bytes are copied; the pack stays in the staging buffer. Never returns. */
	__asm__ volatile (
		"jmp *%4"
		:
		: "a"(kexec_stage), "b"(KEXEC_LOAD), "c"(kernel_size), "d"(entry), "r"(KEXEC_TRAMP)
		: "memory");
}
