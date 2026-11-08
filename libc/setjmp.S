/* i386 setjmp/longjmp for the ring-3 libc.
 *
 * jmp_buf layout: ebx, esi, edi, ebp, esp, eip.
 * setjmp saves the stack pointer after its own return address, so longjmp
 * resumes as if setjmp had returned with a nonzero value. */

	.text
	.globl setjmp
	.type setjmp, @function
setjmp:
	movl	4(%esp), %eax
	movl	%ebx, 0(%eax)
	movl	%esi, 4(%eax)
	movl	%edi, 8(%eax)
	movl	%ebp, 12(%eax)
	leal	4(%esp), %ecx
	movl	%ecx, 16(%eax)
	movl	0(%esp), %ecx
	movl	%ecx, 20(%eax)
	xorl	%eax, %eax
	ret
	.size setjmp, .-setjmp

	.globl longjmp
	.type longjmp, @function
longjmp:
	movl	4(%esp), %edx
	movl	8(%esp), %eax
	testl	%eax, %eax
	jne	1f
	movl	$1, %eax
1:
	movl	0(%edx), %ebx
	movl	4(%edx), %esi
	movl	8(%edx), %edi
	movl	12(%edx), %ebp
	movl	16(%edx), %esp
	jmp	*20(%edx)
	.size longjmp, .-longjmp

	.section .note.GNU-stack,"",@progbits
