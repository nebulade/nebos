/* kexec: hot-reload the kernel over serial without a reboot (C-light variant). */
#ifndef KEXEC_H
#define KEXEC_H

void program_kexec_main(void);

/* Non-zero while a kexec receive is in progress (prevents double-trigger). */
extern volatile int kexec_busy;

/* Receive progress, in bytes. kexec_total is 0 until the header is parsed. */
extern volatile unsigned int kexec_progress;
extern volatile unsigned int kexec_total;

/* Returns 1 if the serial ring buffer starts with the "KXEC" magic, i.e. the
 * host has begun sending a new kernel image. */
int kexec_pending(void);

#endif
