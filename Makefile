# Bootloader: 512-byte boot sector + kernel at sector 2. Builds image.bin for QEMU.

CC      = gcc
AS      = as
LD      = ld
OBJCOPY = objcopy

CFLAGS   = -ffreestanding -fno-pie -m32 -nostdlib -fno-stack-protector \
           -fno-asynchronous-unwind-tables -Wall -Wextra -O2 -c
ASFLAGS  = --32
BOOT_LDFLAGS   = -T linker.ld -nostdlib -m elf_i386
KERNEL_LDFLAGS = -T kernel.ld -nostdlib -m elf_i386

.PHONY: all clean run

all: image.bin

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: boot.o
	$(LD) $(BOOT_LDFLAGS) -o $@ $<

kernel.elf: kernel.o
	$(LD) $(KERNEL_LDFLAGS) -o $@ $<

boot.bin: boot.elf
	$(OBJCOPY) -O binary --only-section=.boot $< $@.tmp
	dd if=$@.tmp of=$@ bs=1 count=510 2>/dev/null
	printf '\x55\xaa' >> $@
	rm -f $@.tmp

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

image.bin: boot.bin kernel.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	dd if=boot.bin of=$@ bs=512 conv=notrunc 2>/dev/null
	dd if=kernel.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null

run: image.bin
	qemu-system-i386 -fda image.bin 2>/dev/null || qemu-system-x86_64 -fda image.bin

clean:
	rm -f boot.o kernel.o boot.elf kernel.elf boot.bin kernel.bin boot.bin.tmp image.bin
