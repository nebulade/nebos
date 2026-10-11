/* Kernel hot-reload over UDP (see netkexec.c). */
#ifndef NETKEXEC_H
#define NETKEXEC_H

/* 1 if a kernel-reload header datagram is waiting (auto-detected from the WM
 * loop).  Returns 0 while a kexec is already running or a net command owns
 * the receiver.  While idle it also answers ARP/ICMP for our IP, so the board
 * stays reachable without running a command. */
int netkexec_pending(void);

/* Process entry: receive the pending image into the kexec staging buffer,
 * verify it and jump.  Never returns on success. */
void program_netkexec_main(void);

#endif
