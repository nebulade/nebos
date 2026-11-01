/*
 * UEFI -> 32-bit kernel handoff trampoline.
 *
 * Linked at 0x800 and copied there by the loader after ExitBootServices.
 * Everything internal uses 0x800-based absolute addresses, so the blob is
 * position-independent until it is copied to its linked address.
 *
 * Entered in 64-bit mode with the Microsoft x64 ABI:
 *   ecx = staging physical address (< 4 GB)
 *   edx = byte count
 *   r8d = kernel_entry address
 *
 * Leaves long mode (compatibility mode, then protected mode), copies the
 * staged image to 0x7E00, and jumps to the kernel with paging off.
 */

.code64
.section .text
.globl _start
_start:
	cli
	movl	%ecx, param_staging(%rip)
	movl	%edx, param_size(%rip)
	movl	%r8d, param_entry(%rip)

	lgdt	gdt_desc
	movl	$0x08, %eax
	pushq	%rax
	movl	$compat, %edx
	pushq	%rdx
	lretq

.code32
compat:
	/* Compatibility mode: 32-bit code, still paging in the firmware's
	 * 64-bit page tables.  Leave IA-32e mode in the documented order. */
	movl	%cr0, %eax
	andl	$0x7FFFFFFF, %eax	/* CR0.PG = 0 */
	movl	%eax, %cr0

	movl	$0xC0000080, %ecx	/* IA32_EFER */
	rdmsr
	andl	$0xFFFFFEFF, %eax	/* LME = 0 */
	wrmsr

	movl	%cr4, %eax
	/* Clear PAE(5), PGE(7), LA57(12), PCIDE(17), SMEP(20), SMAP(21).
	 * The kernel's page tables are non-PAE and its ring-0 code accesses
	 * user pages; the firmware's global TLB entries must not survive. */
	andl	$0xFFCDEF5F, %eax
	movl	%eax, %cr4

	movl	%cr0, %eax
	andl	$0xFFFFFFF3, %eax	/* TS = 0, EM = 0 */
	movl	%eax, %cr0

	ljmp	$0x08, $copy32

copy32:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movl	$0x7C00, %esp

	movl	param_staging, %esi
	movl	$0x7E00, %edi
	movl	param_size, %ecx
	cld
	rep	movsb
	wbinvd

	movl	param_entry, %edx
	jmp	*%edx

.align 8
gdt:
	.quad	0
	.word	0xFFFF, 0x0000
	.byte	0x00, 0x9A, 0xCF, 0x00	/* 0x08: 32-bit code */
	.word	0xFFFF, 0x0000
	.byte	0x00, 0x92, 0xCF, 0x00	/* 0x10: data */
gdt_end:
	.align 2
gdt_desc:
	.word	gdt_end - gdt - 1
	.quad	gdt

.align 4
param_staging:
	.long	0
param_size:
	.long	0
param_entry:
	.long	0
