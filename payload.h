/* payload: replace the command pack over serial at runtime (see payload.c). */
#ifndef PAYLOAD_H
#define PAYLOAD_H

/* Returns 1 when the serial ring buffer starts with the "KXNP" magic. */
int payload_pending(void);

/* Receive + validate + swap the pack; never returns when the board jumps
 * (it does not -- unlike kexec this only replaces /ramfs). */
void program_payload_main(void);

#endif
