/* payload: replace the command pack (ramfs) over serial at runtime.
 *
 * Wire format (same framing as kexec.c, the entry field is unused):
 *   4 bytes: magic "KXNP" (LE)
 *   4 bytes: entry (LE, 0)
 *   4 bytes: size (LE)            -- pack byte count
 *   N bytes: pack data
 *   1 byte:  checksum (sum of data bytes mod 256)
 *
 * The pack is received into page-allocated memory and validated before the
 * swap (ramfs_replace), so a truncated or corrupt transfer leaves the running
 * commands untouched; the previous received copy is freed on success.  The WM
 * idle loop detects the magic like it does for kexec, so `make cmds-dev` on
 * the host alone triggers it. */
#include "payload.h"
#include "io.h"
#include "kexec.h"
#include "mm.h"
#include "ramfs.h"
#include "serial.h"
#include "vfs.h"

#define PAYLOAD_MAGIC 0x504E584B /* "KXNP" little-endian */
#define PAYLOAD_MAX   0x400000   /* sanity cap, like KEXEC_MAX */

int payload_pending(void)
{
	unsigned char b, x, n, p;

	/* Scan for the "KXNP" magic, draining bytes that cannot begin it (same
	 * self-healing as kexec_pending). */
	for (;;) {
		if (!serial_ring_peek(0, &b))
			return 0;
		if (b != 'K') {
			(void)serial_ring_getc();
			continue;
		}
		if (serial_ring_peek(1, &x) && serial_ring_peek(2, &n) &&
		    serial_ring_peek(3, &p)) {
			if (x == 'X' && n == 'N' && p == 'P')
				return 1;
			/* A kernel image ("KXE2", formerly "KXEC")
			 * belongs to kexec_pending(): leave it untouched
			 * (the WM checks this detector first, so consuming
			 * the 'K' here would eat the reload stream). */
			if (x == 'X' && n == 'E' && (p == '2' || p == 'C'))
				return 0;
			(void)serial_ring_getc();
			continue;
		}
		return 0;
	}
}

static unsigned char header[12];

static void free_span(unsigned int base, unsigned int size)
{
	unsigned int i, pages = (size + 0xFFFu) >> 12;

	for (i = 0; i < pages; i++)
		mm_free_page(base + i * 0x1000);
}

static int payload_recv(unsigned int *size, unsigned int *buf)
{
	unsigned int i, n, magic = 0;
	unsigned char c, sum = 0, chk;
	volatile unsigned char *dst;

	for (i = 0; i < sizeof(header); i++) {
		if (!serial_ring_wait(5000, &header[i]))
			goto fail_header;
		if (i < 4)
			magic |= (unsigned int)header[i] << (8 * i);
	}
	if (magic != PAYLOAD_MAGIC)
		goto fail_magic;

	*size = (unsigned int)header[8] | ((unsigned int)header[9] << 8) |
		((unsigned int)header[10] << 16) | ((unsigned int)header[11] << 24);
	if (*size == 0 || *size > PAYLOAD_MAX)
		goto fail_size;

	*buf = mm_alloc_pages((*size + 0xFFFu) >> 12);
	if (!*buf)
		goto fail_mem;
	dst = (volatile unsigned char *)*buf;

	for (n = 0; n < *size; n++) {
		if (!serial_ring_wait(5000, &c)) {
			free_span(*buf, *size);
			goto fail_data;
		}
		dst[n] = c;
		sum += c;
	}
	if (!serial_ring_wait(5000, &chk)) {
		free_span(*buf, *size);
		goto fail_sum;
	}
	if (chk != sum) {
		free_span(*buf, *size);
		goto fail_csum;
	}
	return 0;

fail_header:
	io_putstr("payload: timeout (header)\n");
	return -1;
fail_magic:
	io_putstr("payload: bad magic\n");
	return -1;
fail_size:
	io_putstr("payload: bad size\n");
	return -1;
fail_mem:
	io_putstr("payload: out of memory\n");
	return -1;
fail_data:
	io_putstr("payload: timeout (data)\n");
	return -1;
fail_sum:
	io_putstr("payload: timeout (checksum)\n");
	return -1;
fail_csum:
	io_putstr("payload: checksum mismatch\n");
	return -1;
}

void program_payload_main(void)
{
	unsigned int size = 0, buf = 0;

	io_putstr("payload: waiting for command pack over serial...\n");
	if (payload_recv(&size, &buf) != 0) {
		io_putstr("payload: aborted (pack unchanged)\n");
		kexec_busy = 0;
		return;
	}

	io_putstr("payload: received ");
	io_puthex(size);
	io_putstr(" bytes, ");
	if (ramfs_replace((const void *)buf, size) != 0) {
		io_putstr("not a valid pack (unchanged)\n");
		free_span(buf, size);
	} else {
		vfs_refresh_ramfs();
		io_putstr("pack installed (");
		io_puthex(ramfs_count());
		io_putstr(" files)\n");
	}
	kexec_busy = 0;
}
