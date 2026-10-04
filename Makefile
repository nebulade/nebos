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

.PHONY: all clean run run-floppy run-usb run-serial floppy usb space prog bootstrap dev monitor

all: floppy.img usb.img final.bin root.img

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c idle.h io.h mouse.h process.h terminal.h timer.h serial.h version.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

screen.o: screen.c screen.h
	$(CC) $(CFLAGS) -o $@ $<

io.o: io.c io.h fbtext.h process.h screen.h
	$(CC) $(CFLAGS) -o $@ $<

font_8x8.o: font_8x8.c font_8x8.h
	$(CC) $(CFLAGS) -o $@ $<

fbtext.o: fbtext.c fbtext.h font_8x8.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

terminal.o: terminal.c terminal.h screen.h vfs.h fat.h blkdev.h io.h dbg.h idle.h kexec.h process.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

wm.o: wm.c wm.h terminal.h screen.h fbtext.h idle.h io.h mouse.h process.h rtc.h timer.h version.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

hello.o: hello.c io.h
	$(CC) $(CFLAGS) -o $@ $<

reboot.o: reboot.c
	$(CC) $(CFLAGS) -o $@ $<

poweroff.o: poweroff.c
	$(CC) $(CFLAGS) -o $@ $<

disk.o: disk.c disk.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fat.o: fat.c fat.h blkdev.h io.h
	$(CC) $(CFLAGS) -o $@ $<

ls.o: ls.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

cat.o: cat.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

run.o: run.c fat.h io.h fbtext.h run_api.h timer.h vga.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

rtc.o: rtc.c rtc.h
	$(CC) $(CFLAGS) -o $@ $<

serial.o: serial.c serial.h serial_ring_buf.h pci.h idle.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

serial_ring_buf.o: serial_ring_buf.c serial_ring_buf.h
	$(CC) $(CFLAGS) -o $@ $<

serial_loop.o: serial_loop.c serial.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

bin_recv.o: bin_recv.c bin_recv.h serial.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

serial_recv.o: serial_recv.c serial.h bin_recv.h io.h
	$(CC) $(CFLAGS) -o $@ $<

vga.o: vga.c vga.h pci.h gpu.h ati.h
	$(CC) $(CFLAGS) -o $@ $<

vga_copy.o: vga_copy.S
	$(AS) $(ASFLAGS) -o $@ $<

gpu.o: gpu.c gpu.h pci.h ati.h nvidia.h
	$(CC) $(CFLAGS) -o $@ $<

ati.o: ati.c ati.h pci.h
	$(CC) $(CFLAGS) -o $@ $<

cirrus.o: cirrus.c cirrus.h pci.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

nvidia.o: nvidia.c nvidia.h
	$(CC) $(CFLAGS) -o $@ $<

clock.o: clock.c io.h rtc.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

colors.o: colors.c fbtext.h io.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

display.o: display.c io.h vga.h gpu.h ati.h fbtext.h
	$(CC) $(CFLAGS) -o $@ $<

cpuinfo.o: cpuinfo.c io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

mousecmd.o: mousecmd.c io.h mouse.h
	$(CC) $(CFLAGS) -o $@ $<

uhci.o: uhci.c uhci.h pci.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

usb.o: usb.c usb.h io.h timer.h uhci.h
	$(CC) $(CFLAGS) -o $@ $<

lsusb.o: lsusb.c io.h usb.h
	$(CC) $(CFLAGS) -o $@ $<

lspci.o: lspci.c io.h pci.h
	$(CC) $(CFLAGS) -o $@ $<

ne2000.o: ne2000.c ne2000.h pci.h timer.h io.h
	$(CC) $(CFLAGS) -o $@ $<

netcmd.o: netcmd.c io.h timer.h ne2000.h
	$(CC) $(CFLAGS) -o $@ $<

blkdev.o: blkdev.c blkdev.h ata.h disk.h
	$(CC) $(CFLAGS) -o $@ $<

ata.o: ata.c ata.h pci.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fatdetect.o: fatdetect.c fatdetect.h fat.h blkdev.h
	$(CC) $(CFLAGS) -o $@ $<

storage.o: storage.c blkdev.h vfs.h io.h
	$(CC) $(CFLAGS) -o $@ $<

fatformat.o: fatformat.c fatformat.h blkdev.h
	$(CC) $(CFLAGS) -o $@ $<

format.o: format.c blkdev.h disk.h fatformat.h io.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

mouse.o: mouse.c mouse.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

idle.o: idle.c idle.h
	$(CC) $(CFLAGS) -o $@ $<

kexec.o: kexec.c kexec.h io.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

kexec_tramp.o: kexec_tramp.S
	$(AS) $(ASFLAGS) -o $@ $<

dbg.o: dbg.c dbg.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

save.o: save.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

rm.o: rm.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

mkdir.o: mkdir.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

rmdir.o: rmdir.c fat.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

vfs.o: vfs.c vfs.h fat.h fatdetect.h blkdev.h io.h
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: boot.o
	$(LD) $(BOOT_LDFLAGS) -o $@ $<

vesa_stage.o: vesa_stage.S
	$(AS) $(ASFLAGS) -o $@ $<

timer.o: timer.S
	$(AS) $(ASFLAGS) -o $@ $<

switch.o: switch.S
	$(AS) $(ASFLAGS) -o $@ $<

process.o: process.c process.h io.h
	$(CC) $(CFLAGS) -o $@ $<

kernel.elf: vesa_stage.o kernel.o timer.o switch.o process.o screen.o io.o rtc.o terminal.o wm.o hello.o reboot.o poweroff.o disk.o fat.o ls.o cat.o run.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o vga_copy.o gpu.o ati.o cirrus.o nvidia.o font_8x8.o fbtext.o clock.o colors.o display.o cpuinfo.o mouse.o mousecmd.o uhci.o usb.o lsusb.o blkdev.o ata.o fatdetect.o storage.o fatformat.o format.o vfs.o idle.o kexec.o kexec_tramp.o dbg.o save.o rm.o mkdir.o rmdir.o lspci.o ne2000.o netcmd.o
	$(LD) $(KERNEL_LDFLAGS) -o $@ $^

MBR_LDFLAGS = -T mbr.ld -nostdlib -m elf_i386

mbr.o: mbr.S
	$(AS) $(ASFLAGS) -o $@ $<

mbr.elf: mbr.o
	$(LD) $(MBR_LDFLAGS) -o $@ $<

mbr.bin: mbr.elf
	$(OBJCOPY) -O binary $< $@

boot.bin: boot.elf
	$(OBJCOPY) -O binary --only-section=.boot $< $@.tmp
	dd if=$@.tmp of=$@ bs=1 count=510 2>/dev/null
	printf '\x55\xaa' >> $@
	rm -f $@.tmp

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

KERNEL_SECTORS_OFFSET = 62
BOOT_SECTOR_START_OFFSET = 0x57

floppy.img: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin boot.bin.patched; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=boot.bin.patched bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat boot.bin.patched kernel.bin > $@; \
	truncate -s 1474560 $@; \
	rm -f boot.bin.patched

usb.img: mbr.bin boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin boot.bin.patched; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=boot.bin.patched bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x02' | dd of=boot.bin.patched bs=1 seek=$(BOOT_SECTOR_START_OFFSET) conv=notrunc 2>/dev/null; \
	cat mbr.bin boot.bin.patched kernel.bin > $@; \
	truncate -s 64M $@; \
	rm -f boot.bin.patched

image.bin: floppy.img
	cp floppy.img image.bin

final.bin: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin boot.bin.patched; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=boot.bin.patched bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat boot.bin.patched kernel.bin > $@; \
	rm -f boot.bin.patched

prog.o: prog.c run_api.h
	$(CC) $(CFLAGS) -o $@ $<

prog.elf: prog.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

prog.bin: prog.elf
	$(OBJCOPY) -O binary $< $@

space: space.bin

prog: prog.bin

space.o: space.c run_api.h
	$(CC) $(CFLAGS) -o $@ $<

space.elf: space.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

space.bin: space.elf
	$(OBJCOPY) -O binary $< $@

bootstrap.o: bootstrap.c
	$(CC) $(CFLAGS) -o $@ $<

bootstrap.elf: bootstrap.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

bootstrap.bin: bootstrap.elf
	$(OBJCOPY) -O binary $< $@

root.img: prog.bin space.bin
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	mkfs.fat -F 12 -n "ROOTDISK" $@
	echo "Hello from the second floppy!" | mcopy -i $@ - ::HELLO.TXT
	echo "This is a test file on drive 1." | mcopy -i $@ - ::TEST.TXT
	mcopy -i $@ prog.bin ::PROG.BIN
	mcopy -i $@ space.bin ::SPACE.BIN

run: image.bin root.img
	$(MAKE) run-floppy

floppy: floppy.img

usb: usb.img

run-floppy: floppy.img root.img
	qemu-system-i386 -M pc -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

run-usb: usb.img root.img
	qemu-system-i386 -M pc -boot c -drive file=usb.img,format=raw,if=ide -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -boot c -drive file=usb.img,format=raw,if=ide -drive file=root.img,format=raw,if=floppy

scp-final: final.bin
	scp final.bin root@192.168.0.136:
	ssh root@192.168.0.136 dd if=final.bin of=/dev/fd0 bs=512

scp-root: root.img
	scp root.img root@192.168.0.136:
	ssh root@192.168.0.136 dd if=root.img of=/dev/fd0 bs=512

SERIAL_DEV ?= /dev/ttyUSB0
SERIAL_BAUD ?= 115200
SZ ?= /usr/bin/lrzsz-sz

run-serial:
	@echo "Starting serial monitor on $(SERIAL_DEV) at $(SERIAL_BAUD) baud..."
	@echo "Type 'serial test' or 'serial recv' in the nebos terminal."
	@echo "Press Ctrl-A Ctrl-X to exit picocom."
	picocom $(SERIAL_DEV) --baud $(SERIAL_BAUD)

monitor:
	@echo "Watching $(SERIAL_DEV) @ $(SERIAL_BAUD) baud (Ctrl-A Ctrl-X to exit)."
	@echo "Each 'make dev' reload prints: Nebs OS v<version> booted"
	picocom $(SERIAL_DEV) --baud $(SERIAL_BAUD)

send: $(PROGRAM).bin send_serial
	@echo "Sending $(PROGRAM).bin via serial..."
	./send_serial $(SERIAL_DEV) $(SERIAL_BAUD) $(PROGRAM).bin

send_serial: send_serial.c
	gcc -O2 -o $@ $<

kexec_send: kexec_send.c
	gcc -O2 -o $@ $<

bootstrap: bootstrap.bin

dev: kernel.bin kexec_send
	@entry=$$(nm kernel.elf | awk '/ T kernel_main$$/{print $$1}'); \
	if [ -z "$$entry" ]; then echo "error: kernel_main symbol not found"; exit 1; fi; \
	echo "Sending kernel.bin (entry 0x$$entry) via $(SERIAL_DEV) @ $(SERIAL_BAUD)..."; \
	./kexec_send $(SERIAL_DEV) $(SERIAL_BAUD) $$entry kernel.bin

clean:
	rm -f boot.o vesa_stage.o kernel.o timer.o switch.o process.o screen.o io.o rtc.o terminal.o wm.o hello.o reboot.o poweroff.o fat.o ls.o cat.o run.o disk.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o vga_copy.o gpu.o ati.o cirrus.o nvidia.o font_8x8.o fbtext.o clock.o colors.o display.o cpuinfo.o mouse.o mousecmd.o uhci.o usb.o lsusb.o blkdev.o ata.o fatdetect.o storage.o fatformat.o format.o vfs.o idle.o kexec.o kexec_tramp.o dbg.o save.o rm.o mkdir.o rmdir.o lspci.o ne2000.o netcmd.o mbr.o boot.elf kernel.elf mbr.elf boot.bin mbr.bin kernel.bin boot.bin.tmp boot.bin.patched image.bin floppy.img usb.img final.bin root.img prog.o prog.elf prog.bin space.o space.elf space.bin bootstrap.o bootstrap.elf bootstrap.bin send_serial kexec_send
