/* kexec: hot-reload the kernel over serial without a reboot (C-light variant). */
#ifndef KEXEC_H
#define KEXEC_H

void program_kexec_main(void);

/* Non-zero while a kexec receive is in progress (prevents double-trigger). */
extern volatile int kexec_busy;

/* Returns 1 if the serial ring buffer starts with the "KXEC" magic, i.e. the
 * host has begun sending a new kernel image. */
int kexec_pending(void);

#endif
