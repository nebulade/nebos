/* Serial ring buffer: 4096-byte circular buffer for COM1 receive.
   Single writer (IRQ), single reader (process) — no locks needed. */

#include "serial_ring_buf.h"

static volatile unsigned char ring_buf[SERIAL_RING_SIZE];
static volatile unsigned int ring_head; /* write index, IRQ only */
static volatile unsigned int ring_tail; /* read index, process only */

void serial_ring_init(void)
{
	ring_head = 0;
	ring_tail = 0;
}

void serial_ring_push(unsigned char byte)
{
	unsigned int next = (ring_head + 1) % SERIAL_RING_SIZE;
	if (next != ring_tail) {
		ring_buf[ring_head] = byte;
		ring_head = next;
	}
}

int serial_ring_pop(unsigned char *byte)
{
	if (ring_tail == ring_head)
		return 0;
	*byte = ring_buf[ring_tail];
	ring_tail = (ring_tail + 1) % SERIAL_RING_SIZE;
	return 1;
}

unsigned int serial_ring_count(void)
{
	unsigned int h = ring_head;
	unsigned int t = ring_tail;
	if (h >= t)
		return h - t;
	return SERIAL_RING_SIZE - t + h;
}
