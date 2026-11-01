/* Flat kernel image and the 0x800 trampoline, embedded in BOOTX64.EFI. */

.section .data,"dw"
.globl kernel_image_start
.globl kernel_image_end
.align 16
kernel_image_start:
.incbin "kernel.bin"
kernel_image_end:

.align 16
.globl tramp_blob_start
.globl tramp_blob_end
tramp_blob_start:
.incbin "efi/tramp.bin"
tramp_blob_end:
