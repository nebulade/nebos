/* IRQ_RETURN_DS <cs_offset>
 *
 * Load DS/ES/FS/GS for the context we are about to IRET to, in a stub that
 * switched them to the kernel data selector (0x10) to run C code.
 *
 * Why: an IRET to a less privileged level nulls DS/ES/FS/GS whose DPL is
 * lower than the new CPL.  Coming back to ring 3 with 0x10 (DPL 0) therefore
 * leaves the process with a *null* DS, and user code #GPs on its first data
 * access (even a simple stack-buffer store through a pointer).  QEMU does not
 * model this nulling, so it only shows up on real hardware.
 *
 * So: ring-3 contexts get the user data selector (0x23), ring-0 contexts keep
 * 0x10.  <cs_offset> is the byte offset of the saved CS in the current frame
 * (pusha is 32 bytes, then the CPU's EIP/CS/EFLAGS: offset 36).  Clobbers %cx,
 * which the caller restores via popa.
 */
.macro IRQ_RETURN_DS cs_offset
	movw  \cs_offset(%esp), %cx
	andw  $3, %cx
	cmpw  $3, %cx
	jne   1f
	movw  $0x23, %cx
	jmp   2f
1:
	movw  $0x10, %cx
2:
	movw  %cx, %ds
	movw  %cx, %es
	movw  %cx, %fs
	movw  %cx, %gs
.endm
