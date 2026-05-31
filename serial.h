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
int serial_has_data(unsigned int port);
char serial_getc_nb(unsigned int port);

extern unsigned int serial_recv_size;

#endif
