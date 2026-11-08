/* Minimal SNTP client (RFC 4330): see ntp.c. */
#ifndef NTP_H
#define NTP_H

#include "syscall.h"

/* Server used when the caller passes an empty name. */
#define NTP_DEFAULT_SERVER "pool.ntp.org"

/* Resolve `server` (a dotted address skips DNS), ARP the next hop, send one
 * NTP client request and wait for the reply.  Returns a SYS_NTP_* code and,
 * on success, fills `out` with the server's UTC transmit time.  The caller
 * must own the receiver (sys_ntp_sync() takes care of that for ring 3). */
int ntp_sync(const char *server, unsigned int timeout_ms,
	     struct sys_datetime *out);

#endif
