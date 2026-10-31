/* Fatal exception screen.
 *
 * Only ring-0 faults reach this path: a fault from ring 3 kills the offending
 * process and the kernel keeps running (see exc_handler() in kernel.c).  The
 * handler runs with interrupts off and never returns, so this code must not
 * depend on the scheduler, the heap or the current screen buffer: it draws
 * straight into the VESA LFB (bypassing the WM and the shadow buffer) and
 * mirrors everything to COM1.
 *
 * Addresses printed here are raw kernel addresses; resolve them with
 * `nm kernel.elf` / `addr2line -e kernel.elf`. */
#ifndef PANIC_H
#define PANIC_H

/* CPU state captured by the exception stubs (timer.S).  The layout must
 * match the stub exactly, low to high:
 *
 *   pusha block: EDI, ESI, EBP, ESP(as saved by pusha), EBX, EDX, ECX, EAX
 *   exception frame: vector, error code, EIP, CS, EFLAGS, [user ESP, user SS]
 *
 * The ESP/SS fields are only meaningful for a fault from ring 3 (the CPU
 * pushes them on a privilege change). */
struct exc_state {
	unsigned int edi, esi, ebp, esp_pusha;
	unsigned int ebx, edx, ecx, eax;
	unsigned int vector, err, eip, cs, eflags, esp, ss;
};

/* Draw the banner and dump the state to COM1.  `name` is the vector name. */
void panic_report(const struct exc_state *st, const char *name);

#endif
