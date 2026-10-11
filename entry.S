/* Canonical kernel entry: the first instruction of the kernel for every boot
 * path (vesa_stage.S jumps here; the kexec paths use this symbol as the
 * transfer entry, see the dev/net-dev targets).
 *
 * It exists so a kernel reload cannot inherit a stale stack: the jumping
 * kernel's trampoline (kexec_tramp.S) sets ESP from a constant that was baked
 * into the *old* build's memory map.  If the map moved in between (the kernel
 * region grew), that stack can sit inside the new image and the BSS clear
 * would wipe the stack it is running on.  Re-pointing ESP here, before any
 * stack use, makes every reload independent of the old layout. */
.code32
.text

.globl kernel_entry
kernel_entry:
	movl  $0x49800, %esp
	jmp   kernel_main
