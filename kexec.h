/* kexec: hot-reload the kernel over serial without a reboot (C-light variant). */
#ifndef KEXEC_H
#define KEXEC_H

void program_kexec_main(void);

/* Kernel load address (the boot sector's) and a sanity cap for an incoming
 * image.  The staging buffer itself is allocated on demand: it used to be a
 * fixed region below the VGA window, which forced the low-memory map to move
 * whenever the kernel grew. */
#define KEXEC_LOAD  0x7E00
#define KEXEC_MAX   0x400000

/* Allocate the staging buffer for an image of `size` bytes (contiguous,
 * page-aligned, identity-mapped).  Sets kexec_stage; returns 1 on success. */
int kexec_stage_alloc(unsigned int size);

/* Free a staging buffer allocated by kexec_stage_alloc(). */
void kexec_stage_free(void);

/* Physical base of the staging buffer holding the received image. */
extern unsigned int kexec_stage;

/* Copy the staged kernel (`kernel_size` bytes) over the kernel region,
 * relocate the trampoline and jump to `entry`.  `pack_addr`/`pack_size`
 * describe the boot-time rootfs source; a zero pack_size means "keep the
 * running ramfs".  Never returns. */
void kexec_jump(unsigned int entry, unsigned int kernel_size,
		unsigned int pack_addr, unsigned int pack_size);

/* Non-zero while a kexec receive is in progress (prevents double-trigger). */
extern volatile int kexec_busy;

/* Receive progress, in bytes. kexec_total is 0 until the header is parsed. */
extern volatile unsigned int kexec_progress;
extern volatile unsigned int kexec_total;

/* Returns 1 if the serial ring buffer starts with the "KXE2" magic, i.e. the
 * host has begun sending a new kernel/rootfs image. */
int kexec_pending(void);

#endif
