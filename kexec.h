/* kexec: hot-reload the kernel over serial without a reboot (C-light variant). */
#ifndef KEXEC_H
#define KEXEC_H

void program_kexec_main(void);

/* Staging buffer for a received kernel image, and the maximum image size
 * (the kernel region 0x7E00..0x42000).  Below the VGA window; see AGENTS.md. */
#define KEXEC_STAGE 0x58000
#define KEXEC_LOAD  0x7E00
#define KEXEC_MAX   0x3A200

/* Copy the staged image (KEXEC_STAGE, `size` bytes) over the kernel region,
 * relocate the trampoline and jump to `entry`.  Never returns. */
void kexec_jump(unsigned int entry, unsigned int size);

/* Non-zero while a kexec receive is in progress (prevents double-trigger). */
extern volatile int kexec_busy;

/* Receive progress, in bytes. kexec_total is 0 until the header is parsed. */
extern volatile unsigned int kexec_progress;
extern volatile unsigned int kexec_total;

/* Returns 1 if the serial ring buffer starts with the "KXEC" magic, i.e. the
 * host has begun sending a new kernel image. */
int kexec_pending(void);

#endif
