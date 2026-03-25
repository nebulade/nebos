/* Serial port (COM1) debug output. */
#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(unsigned int n);
void serial_putdec(unsigned int n);

#endif
