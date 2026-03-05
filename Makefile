# Minimal x86 floppy boot: build boot.bin and optionally write to floppy.
# Usage: make [all] && [sudo] make floppy

CC     = gcc
AS     = as
LD     = ld
OBJCOPY = objcopy

CFLAGS  = -ffreestanding -fno-pie -m16 -nostdlib -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -O2 -c
ASFLAGS = --32
LDFLAGS = -T linker.ld -nostdlib -m elf_i386

FLOPPY ?= /dev/fd0

OBJS = boot.o kernel.o

.PHONY: all clean floppy run

all: boot.bin

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

boot.bin: boot.elf
	$(OBJCOPY) -O binary --only-section=.boot $< $@.tmp
	dd if=$@.tmp of=$@ bs=1 count=510 2>/dev/null
	printf '\x55\xaa' >> $@
	rm -f $@.tmp

# Write first sector to floppy (often requires: sudo make floppy)
floppy: boot.bin
	dd if=boot.bin of=$(FLOPPY) bs=512 count=1 conv=notrunc

# Test in QEMU (optional: install qemu-system-i386 or qemu-system-x86)
# Use -display gtk for a window; install qemu-system-gtk or run in a graphical session
run: boot.bin
	qemu-system-i386 -display gtk -drive file=boot.bin,format=raw,if=floppy || qemu-system-x86_64 -display gtk -drive file=boot.bin,format=raw,if=floppy

clean:
	rm -f $(OBJS) boot.elf boot.bin boot.bin.tmp
