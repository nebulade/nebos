/* int 0x80 syscall stub (ring-3 -> ring-0).
 *
 * On entry the CPU has pushed SS, ESP, EFLAGS, CS, EIP (from ring-3).
 * Registers: eax = syscall number, ebx/ecx/edx = args 1..3.
 * The dispatcher's return value is placed in eax before returning to user. */
.code32
.text

.include "irqret.inc"

.globl syscall_stub
syscall_stub:
	/* Disable interrupts for the duration of the syscall: the trap gate keeps
	 * IF=1, so otherwise the PIT could fire and the context switch would switch
	 * the stack out from under us mid-syscall.  Blocking syscalls (sleep/exit)
	 * re-enable interrupts themselves. */
	cli

	/* Load the kernel selectors: DS/ES/FS = 0x10, GS = 0x28 (the per-CPU
	 * data selector used by this_cpu()).  int 0x80 only switches CS and SS;
	 * IRET reset DS (etc.) to null when it returned to ring-3, so DS is null
	 * here and the C dispatcher's data accesses would #GP. */
	pushl %eax
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %fs
	movw  $0x28, %ax
	movw  %ax, %gs
	popl  %eax

	pusha

	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	call  syscall_dispatch
	addl  $16, %esp
	movl  %eax, 28(%esp)   /* overwrite the saved eax with the return value */

	/* Returning to the ring-3 caller: leave it a usable data selector (the
	 * IRET would null 0x10, and user code #GPs on its first data access).
	 * See irqret.inc for the why; this gate is ring-3-only. */
	IRQ_RETURN_DS 36

	popa
	iret
