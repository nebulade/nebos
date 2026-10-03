/* Serial port (COM1/COM2) debug output. */
#ifndef SERIAL_H
#define SERIAL_H

#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8

void serial_init(unsigned int port);
void serial_putc(unsigned int port, char c);
void serial_puts(unsigned int port, const char *s);
void serial_puthex(unsigned int port, unsigned int n);
void serial_putdec(unsigned int port, unsigned int n);
int  serial_has_data(unsigned int port);
void serial_poll(void);
void serial_ring_init(void);
int  serial_ring_getc(void);
int  serial_ring_wait(unsigned int timeout_ms, unsigned char *out);
int  serial_ring_peek(unsigned int offset, unsigned char *byte);

extern unsigned int serial_recv_size;

#endif
