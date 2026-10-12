/* Minimal TLS 1.3 client for ring-3 commands (see tls.c).
 *
 * TLS_CHACHA20_POLY1305_SHA256 + X25519 only.  Certificates are NOT
 * verified (phase A): the leaf fingerprint is printed so a human can
 * compare it out of band.  The TCP connection must already be up; the
 * caller owns the NIC. */
#ifndef TLS_H
#define TLS_H

/* 0 on success (then tls_send/tls_recv may be used), -1 on failure. */
int tls_connect(const char *sni);

/* 0 on success; the payload is sent as one record (<= 1024 bytes). */
int tls_send(const void *buf, unsigned int len);

/* >0 bytes, 0 = peer closed (close_notify or TCP EOF), -1 = timeout,
 * -2 = reset/protocol error.  Same shape as SYS_TCP_RECV. */
int tls_recv(void *buf, unsigned int max, unsigned int timeout_ms);

/* Best-effort close_notify (then the caller closes the TCP connection). */
void tls_close(void);

#endif
