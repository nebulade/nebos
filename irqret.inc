/* IRQ_RETURN_DS <cs_offset>
 *
 * Load DS/ES/FS/GS for the context we are about to IRET to, in a stub that
 * switched them to the kernel selectors to run C code.
 *
 * Why: an IRET to a less privileged level nulls DS/ES/FS/GS whose DPL is
 * lower than the new CPL.  Coming back to ring 3 with 0x10 (DPL 0) therefore
 * leaves the process with a *null* DS, and user code #GPs on its first data
 * access (even a simple stack-buffer store through a pointer).  QEMU does not
 * model this nulling, so it only shows up on real hardware.
 *
 * Ring-3 contexts get the user data selector (0x23).  Ring-0 contexts get
 * 0x10 for DS/ES/FS and 0x28 for GS: GS is the per-CPU data selector whose
 * base points at this CPU's struct cpu, and this_cpu()/current_proc() read
 * through it.  <cs_offset> is the byte offset of the saved CS in the current
 * frame (pusha is 32 bytes, then the CPU's EIP/CS/EFLAGS: offset 36).
 * Clobbers %cx, which the caller restores via popa.
 */
.macro IRQ_RETURN_DS cs_offset
	movw  \cs_offset(%esp), %cx
	andw  $3, %cx
	cmpw  $3, %cx
	jne   1f
	movw  $0x23, %cx
	movw  %cx, %ds
	movw  %cx, %es
	movw  %cx, %fs
	movw  %cx, %gs
	jmp   2f
1:
	movw  $0x10, %cx
	movw  %cx, %ds
	movw  %cx, %es
	movw  %cx, %fs
	movw  $0x28, %cx
	movw  %cx, %gs
2:
.endm
