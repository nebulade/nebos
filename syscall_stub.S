/* int 0x80 syscall stub (ring-3 -> ring-0).
 *
 * On entry the CPU has pushed SS, ESP, EFLAGS, CS, EIP (from ring-3).
 * Registers: eax = syscall number, ebx/ecx/edx = args 1..3.
 * The dispatcher's return value is placed in eax before returning to user. */
.code32
.text

.globl syscall_stub
syscall_stub:
	pusha
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	call  syscall_dispatch
	addl  $16, %esp
	movl  %eax, 28(%esp)   /* overwrite the saved eax with the return value */
	popa
	iret
