/* netlog: mirror kernel debug output (COM1) and terminal text to UDP, so a
 * serial-less board has a `make monitor` equivalent (`make netmon`).
 *
 * Producers (serial_putc / screen_putchar) only append to a RAM ring buffer;
 * the WM loop flushes complete lines (or after a short idle delay) as UDP
 * datagrams.  Nothing is sent before the NIC is up, and nothing blocks the
 * producer. */
#ifndef NETLOG_H
#define NETLOG_H

/* UDP port both the mirror and the `netlog` command use. */
#define NETLOG_PORT 19290

/* Enable the mirror and clear the buffer; call at boot (before the NIC is
 * initialized, so the driver's bring-up trail is captured). */
void netlog_init(void);

/* Producer: append one character.  Cheap and safe from any context (IRQ,
 * boot, process); drops the character when the ring is full. */
void netlog_char(char c);

/* Consumer: send pending text as one datagram (call from the WM loop). */
void netlog_flush(void);

void netlog_set_enabled(int on);
int  netlog_get_enabled(void);

/* Destination: all-0xFF (default) broadcasts; otherwise datagrams carry the
 * broadcast MAC and this IP, so no ARP cache is needed (the switch floods
 * them and the host with that address presents them to its sockets). */
void netlog_set_host(const unsigned char ip[4]);
void netlog_get_host(unsigned char ip[4]);

#endif
