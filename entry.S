/* Canonical kernel entry: the first instruction of the kernel for every boot
 * path (vesa_stage.S jumps here; the kexec paths use this symbol as the
 * transfer entry, see the dev/net-dev targets).
 *
 * It exists so a kernel reload cannot inherit a stale stack: the jumping
 * kernel's trampoline (kexec_tramp.S) copies the new image over this one
 * while running on its own private stack, then jumps here.  Re-pointing ESP
 * before any stack use makes the reload independent of where either build
 * put its stacks. */
.code32
.text

.globl kernel_entry
kernel_entry:
	movl  $__boot_stack_top, %esp
	jmp   kernel_main
