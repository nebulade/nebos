# Bootloader: 512-byte boot sector + contiguous kernel at sector 1. Builds image.bin for QEMU.

CC      = gcc
AS      = as
LD      = ld
OBJCOPY = objcopy

# -march=i586: first-gen Pentium (P5); avoids CMOV etc. (P6+)
CFLAGS   = -ffreestanding -fno-pie -m32 -march=i586 -nostdlib -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-sse2 -mno-mmx \
           -Wall -Wextra -O2 -c
ASFLAGS  = --32
BOOT_LDFLAGS   = -T linker.ld -nostdlib -m elf_i386
KERNEL_LDFLAGS = -T kernel.ld -nostdlib -m elf_i386

.PHONY: all clean run

all: image.bin final.bin root.img

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c io.h terminal.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

io.o: io.c io.h fbtext.h
	$(CC) $(CFLAGS) -o $@ $<

font_8x8.o: font_8x8.c font_8x8.h
	$(CC) $(CFLAGS) -o $@ $<

fbtext.o: fbtext.c fbtext.h font_8x8.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

terminal.o: terminal.c terminal.h io.h fbtext.h rtc.h timer.h disk.h
	$(CC) $(CFLAGS) -o $@ $<

hello.o: hello.c io.h
	$(CC) $(CFLAGS) -o $@ $<

reboot.o: reboot.c
	$(CC) $(CFLAGS) -o $@ $<

disk.o: disk.c disk.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fat.o: fat.c fat.h disk.h
	$(CC) $(CFLAGS) -o $@ $<

ls.o: ls.c fat.h io.h
	$(CC) $(CFLAGS) -o $@ $<

cat.o: cat.c fat.h io.h
	$(CC) $(CFLAGS) -o $@ $<

run.o: run.c fat.h io.h fbtext.h prog_api.h
	$(CC) $(CFLAGS) -o $@ $<

rtc.o: rtc.c rtc.h
	$(CC) $(CFLAGS) -o $@ $<

vga.o: vga.c vga.h pci.h ati.h
	$(CC) $(CFLAGS) -o $@ $<

ati.o: ati.c ati.h pci.h
	$(CC) $(CFLAGS) -o $@ $<

clock.o: clock.c io.h rtc.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

colors.o: colors.c fbtext.h io.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

display.o: display.c io.h vga.h ati.h fbtext.h
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: boot.o
	$(LD) $(BOOT_LDFLAGS) -o $@ $<

timer.o: timer.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.elf: kernel.o timer.o io.o rtc.o terminal.o hello.o reboot.o disk.o fat.o ls.o cat.o run.o vga.o ati.o font_8x8.o fbtext.o clock.o colors.o display.o
	$(LD) $(KERNEL_LDFLAGS) -o $@ $^

boot.bin: boot.elf
	$(OBJCOPY) -O binary --only-section=.boot $< $@.tmp
	dd if=$@.tmp of=$@ bs=1 count=510 2>/dev/null
	printf '\x55\xaa' >> $@
	rm -f $@.tmp

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

KERNEL_SECTORS_OFFSET = 62
image.bin: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin boot.bin.patched; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=boot.bin.patched bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat boot.bin.patched kernel.bin > $@; \
	truncate -s 1474560 $@; \
	rm -f boot.bin.patched

final.bin: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin boot.bin.patched; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=boot.bin.patched bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat boot.bin.patched kernel.bin > $@; \
	rm -f boot.bin.patched

prog.o: prog.c prog_api.h
	$(CC) $(CFLAGS) -o $@ $<

prog.elf: prog.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

prog.bin: prog.elf
	$(OBJCOPY) -O binary $< $@

root.img: prog.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	mkfs.fat -F 12 -n "ROOTDISK" $@
	echo "Hello from the second floppy!" | mcopy -i $@ - ::HELLO.TXT
	echo "This is a test file on drive 1." | mcopy -i $@ - ::TEST.TXT
	mcopy -i $@ prog.bin ::PROG.BIN

run: image.bin root.img
	qemu-system-i386 -M pc -drive file=image.bin,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -drive file=image.bin,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

clean:
	rm -f boot.o kernel.o timer.o io.o rtc.o terminal.o hello.o reboot.o fat.o ls.o cat.o run.o disk.o vga.o ati.o font_8x8.o fbtext.o clock.o colors.o display.o boot.elf kernel.elf boot.bin kernel.bin boot.bin.tmp boot.bin.patched image.bin final.bin root.img prog.o prog.elf prog.bin
