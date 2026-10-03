/* Position-independent kexec trampoline. Copied to 0x800 at runtime (see
 * kexec.c) because the final copy overwrites the kernel code we're executing.
 *
 * Args in registers (set by kexec.c):
 *   eax = source (staging buffer)
 *   ebx = dest   (0x7E00)
 *   ecx = byte count
 *   edx = entry  (new kernel_main)
 *
 * Also rebuilds a flat GDT at 0x900 and loads it, so the new kernel's segment
 * reloads (iret on the first context switch) don't read the old GDT that lives
 * inside the kernel region we overwrite. 0x900 is below 0x7E00 and is never
 * overwritten, so the GDT survives. Clobbers esi/edi. Never returns.
 */
.code32
.text

.globl kexec_trampoline
kexec_trampoline:
	cli
	pushl %eax
	pushl %ebx
	pushl %ecx
	pushl %edx

	/* Mask all IRQs. */
	movb  $0xFF, %al
	outb  %al, $0x21
	outb  %al, $0xA1

	/* Build flat GDT (base 0 / 4 GB) at 0x900 and load it. */
	movl  $0x900, %edi
	movl  $0, 0(%edi)          /* null */
	movl  $0, 4(%edi)
	movl  $0x0000FFFF, 8(%edi) /* code: limit 0xFFFF, base low 0 */
	movl  $0x00CF9A00, 12(%edi)/* code: base mid 0, 0x9A, 0xCF, base hi 0 */
	movl  $0x0000FFFF, 16(%edi)/* data: limit 0xFFFF, base low 0 */
	movl  $0x00CF9200, 20(%edi)/* data: base mid 0, 0x92, 0xCF, base hi 0 */
	movw  $0x17, 24(%edi)      /* gdtr limit = 23 */
	movl  $0x900, 26(%edi)     /* gdtr base = 0x900 */
	lgdt  24(%edi)

	popl  %edx
	popl  %ecx
	popl  %ebx
	popl  %eax

	/* Copy new kernel: src=eax -> dst=ebx, count=ecx. */
	movl  %eax, %esi
	movl  %ebx, %edi
	cld
	rep   movsb

	movl  $0x30800, %esp
	jmp   *%edx

.globl kexec_trampoline_end
kexec_trampoline_end:
	nop
