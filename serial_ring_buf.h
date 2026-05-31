/* Serial ring buffer: IRQ-driven receive for COM1. */
#ifndef SERIAL_RING_BUF_H
#define SERIAL_RING_BUF_H

#define SERIAL_RING_SIZE 4096

void serial_ring_init(void);
void serial_ring_push(unsigned char byte);
int  serial_ring_pop(unsigned char *byte);
unsigned int serial_ring_count(void);

#endif
