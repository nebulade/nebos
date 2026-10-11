/* kexec: receive a new kernel.bin over serial and jump into it.
 *
 * C-light reload: we do NOT return to real mode or re-run the VESA stage.
 * Instead we reuse the current framebuffer (the boot FB info at 0x600 and the
 * LFB are still valid) and jump straight to the new kernel's kernel_main,
 * which re-inits IDT/PIC/PIT/mouse/process/VGA-shadow/terminal from scratch.
 *
 * Because this code lives at 0x7E00..0x40000 (inside the region we overwrite),
 * the final "copy image to 0x7E00 and jump" is done by a small
 * position-independent trampoline relocated to 0x800 (below the kernel).
 *
 * The terminal polls kexec_pending() on its idle loop, so `make dev` on the
 * host alone triggers the reload (no typing required). The manual `kexec`
 * command is kept as a fallback that waits for the same image.
 *
 * Wire protocol (host -> us):
 *   4 bytes: magic "KXEC" (LE)
 *   4 bytes: entry address (LE)   -- new kernel_main VMA
 *   4 bytes: size (LE)            -- kernel.bin byte count
 *   N bytes: kernel.bin data
 *   1 byte:  checksum (sum of data bytes mod 256)
 */

#include "kexec.h"
#include "io.h"
#include "serial.h"

#define KEXEC_MAGIC 0x4345584B /* "KXEC" little-endian */

/* The staging buffer must hold a full kernel image and stay below the VGA
   window (0xA0000). */
#if KEXEC_STAGE + KEXEC_MAX > 0xA0000
#error "kexec staging overlaps the VGA aperture"
#endif

#define KEXEC_TRAMP 0x800      /* relocated trampoline (below the kernel) */

volatile int kexec_busy;
volatile unsigned int kexec_progress;
volatile unsigned int kexec_total;

extern unsigned char kexec_trampoline[];
extern unsigned char kexec_trampoline_end[];

int kexec_pending(void)
{
	unsigned char b, x, e, c;

	/* Scan for the "KXEC" magic, draining any leading bytes that can't begin
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
			if (x == 'X' && e == 'E' && c == 'C')
				return 1;         /* "KXEC" found */
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

static int kexec_recv(unsigned int *entry, unsigned int *size)
{
	volatile unsigned char *dst = (volatile unsigned char *)KEXEC_STAGE;
	unsigned int i, magic = 0, n;
	unsigned char c, sum = 0, chk;

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

	*size = 0;
	for (i = 0; i < 4; i++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (size)\n");
		*size |= (unsigned int)c << (8 * i);
	}
	if (*size == 0 || *size > KEXEC_MAX)
		return kexec_fail("kexec: bad size\n");
	kexec_total = *size;
	kexec_progress = 0;

	for (n = 0; n < *size; n++) {
		if (!serial_ring_wait(5000, &c))
			return kexec_fail("kexec: timeout (data)\n");
		dst[n] = c;
		sum += c;
		kexec_progress = n + 1;
		if ((n & 0xFFF) == 0xFFF)
			io_putchar('.');
	}
	io_putchar('\n');

	if (!serial_ring_wait(5000, &chk))
		return kexec_fail("kexec: timeout (checksum)\n");
	if (chk != sum)
		return kexec_fail("kexec: checksum mismatch\n");

	return 0;
}

void program_kexec_main(void)
{
	unsigned int entry = 0, size = 0;

	io_putstr("kexec: waiting for kernel image over serial...\n");
	if (kexec_recv(&entry, &size) != 0) {
		io_putstr("kexec: aborted\n");
		kexec_busy = 0;
		return;
	}

	io_putstr("kexec: received ");
	io_puthex(size);
	io_putstr(" bytes, entry ");
	io_puthex(entry);
	io_putstr("\n");

	kexec_jump(entry, size);
}

/* Relocate the trampoline to 0x800 (it survives the kernel overwrite) and
 * jump to it with the staging/load/size/entry arguments.  Never returns. */
void kexec_jump(unsigned int entry, unsigned int size)
{
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

	/* Jump to the trampoline with args in eax/ebx/ecx/edx. Never returns. */
	__asm__ volatile (
		"jmp *%4"
		:
		: "a"(KEXEC_STAGE), "b"(KEXEC_LOAD), "c"(size), "d"(entry), "r"(KEXEC_TRAMP)
		: "memory");
}
