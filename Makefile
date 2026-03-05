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

kernel.o: kernel.c io.h terminal.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

io.o: io.c io.h
	$(CC) $(CFLAGS) -o $@ $<

terminal.o: terminal.c terminal.h io.h rtc.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

hello.o: hello.c io.h
	$(CC) $(CFLAGS) -o $@ $<

rtc.o: rtc.c rtc.h
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: boot.o
	$(LD) $(BOOT_LDFLAGS) -o $@ $<

timer.o: timer.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.elf: kernel.o timer.o io.o rtc.o terminal.o hello.o
	$(LD) $(KERNEL_LDFLAGS) -o $@ $^

boot.bin: boot.elf
	$(OBJCOPY) -O binary --only-section=.boot $< $@.tmp
	dd if=$@.tmp of=$@ bs=1 count=510 2>/dev/null
	printf '\x55\xaa' >> $@
	rm -f $@.tmp

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

image.bin: boot.bin kernel.bin
	@KERNEL_SECTORS=$$(( ($$(wc -c < kernel.bin) + 511) / 512 )); \
	TOTAL_SECTORS=$$(( 1 + KERNEL_SECTORS )); \
	dd if=/dev/zero of=$@ bs=512 count=$$TOTAL_SECTORS 2>/dev/null; \
	dd if=boot.bin of=$@ bs=512 conv=notrunc 2>/dev/null; \
	dd if=kernel.bin of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null

run: image.bin
	qemu-system-i386 -M pc -drive file=image.bin,format=raw 2>/dev/null || qemu-system-x86_64 -M pc -drive file=image.bin,format=raw

clean:
	rm -f boot.o kernel.o timer.o io.o rtc.o terminal.o hello.o boot.elf kernel.elf boot.bin kernel.bin boot.bin.tmp image.bin
