/* Flat kernel image, the boot rootfs pack and the 0x800 trampoline, embedded
 * in BOOTX64.EFI. */

.section .data,"dw"
.globl kernel_image_start
.globl kernel_image_end
.align 16
kernel_image_start:
.incbin "kernel.bin"
kernel_image_end:

.globl pack_image_start
.globl pack_image_end
.align 16
pack_image_start:
.incbin "cmds_pack_gz.bin"
pack_image_end:

.align 16
.globl tramp_blob_start
.globl tramp_blob_end
tramp_blob_start:
.incbin "efi/tramp.bin"
tramp_blob_end:
