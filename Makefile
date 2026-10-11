# Bootloader: 512-byte boot sector + contiguous kernel at sector 1. Builds image.bin for QEMU.

CC      = gcc
AS      = as
LD      = ld
OBJCOPY = objcopy

# -march=i586: first-gen Pentium (P5); avoids CMOV etc. (P6+)
CFLAGS   = -ffreestanding -fno-pie -m32 -march=i586 -nostdlib -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-sse2 -mno-mmx \
           -Wall -Wextra -O2 -c
ASFLAGS  = --32 -I.
BOOT_LDFLAGS   = -T linker.ld -nostdlib -m elf_i386
KERNEL_LDFLAGS = -T kernel.ld -nostdlib -m elf_i386

.PHONY: all clean run run-floppy run-usb run-uhci run-net run-ahci test-usb run-serial floppy usb flash space prog bootstrap dev net-dev netmon monitor

all: floppy.img usb.img final.bin root.img

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c idle.h io.h mouse.h process.h terminal.h timer.h serial.h usbhid.h version.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

screen.o: screen.c screen.h
	$(CC) $(CFLAGS) -o $@ $<

io.o: io.c io.h fbtext.h process.h screen.h
	$(CC) $(CFLAGS) -o $@ $<

font_8x8.o: font_8x8.c font_8x8.h
	$(CC) $(CFLAGS) -o $@ $<

fbtext.o: fbtext.c fbtext.h font_8x8.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

terminal.o: terminal.c terminal.h screen.h vfs.h fat.h blkdev.h io.h dbg.h kexec.h process.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

wm.o: wm.c wm.h terminal.h screen.h fbtext.h idle.h io.h mouse.h process.h rtc.h timer.h version.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

hello.o: hello.c
	$(CC) $(CFLAGS) -o $@ $<

reboot.o: reboot.c
	$(CC) $(CFLAGS) -o $@ $<

poweroff.o: poweroff.c
	$(CC) $(CFLAGS) -o $@ $<

disk.o: disk.c disk.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fat.o: fat.c fat.h blkdev.h io.h
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

gpu.o: gpu.c gpu.h pci.h ati.h cirrus.h nvidia.h
	$(CC) $(CFLAGS) -o $@ $<

ati.o: ati.c ati.h pci.h radeon.h
	$(CC) $(CFLAGS) -o $@ $<

cirrus.o: cirrus.c cirrus.h pci.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

nvidia.o: nvidia.c nvidia.h nv50.h pci.h process.h serial.h
	$(CC) $(CFLAGS) -Os -o $@ $<

nv50.o: nv50.c nv50.h nvidia.h serial.h
	$(CC) $(CFLAGS) -Os -o $@ $<

radeon.o: radeon.c radeon.h pci.h paging.h serial.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

mtrr.o: mtrr.c mtrr.h kdbg.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

clock.o: clock.c io.h rtc.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

colors.o: colors.c fbtext.h io.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

display.o: display.c io.h vga.h gpu.h ati.h nvidia.h radeon.h mouse.h fbtext.h
	$(CC) $(CFLAGS) -o $@ $<

uhci.o: uhci.c uhci.h usb_hcd.h pci.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

ohci.o: ohci.c ohci.h usb_hcd.h pci.h paging.h mm.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

usb.o: usb.c usb.h usb_hcd.h io.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

usbhid.o: usbhid.c usbhid.h io.h usb.h usb_hcd.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

lsusb.o: lsusb.c io.h usb.h
	$(CC) $(CFLAGS) -o $@ $<

kdbg.o: kdbg.c kdbg.h font_8x8.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

lspci.o: lspci.c io.h pci.h
	$(CC) $(CFLAGS) -o $@ $<

peek.o: peek.c io.h
	$(CC) $(CFLAGS) -o $@ $<

ne2000.o: ne2000.c ne2000.h pci.h timer.h io.h
	$(CC) $(CFLAGS) -o $@ $<

r8168.o: r8168.c r8168.h netdev.h io.h kdbg.h mm.h paging.h pci.h process.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

netcmd.o: netcmd.c io.h timer.h netdev.h net.h
	$(CC) $(CFLAGS) -o $@ $<

netkexec.o: netkexec.c netkexec.h io.h kexec.h net.h netdev.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

netlog.o: netlog.c netlog.h net.h netdev.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

dhcp.o: dhcp.c dhcp.h io.h net.h netdev.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

dns.o: dns.c dns.h io.h timer.h netdev.h net.h
	$(CC) $(CFLAGS) -o $@ $<

tcp.o: tcp.c tcp.h io.h timer.h netdev.h net.h
	$(CC) $(CFLAGS) -o $@ $<

curl.o: curl.c io.h timer.h netdev.h net.h dns.h tcp.h
	$(CC) $(CFLAGS) -o $@ $<

netdev.o: netdev.c netdev.h ne2000.h
	$(CC) $(CFLAGS) -o $@ $<

net.o: net.c net.h io.h netdev.h
	$(CC) $(CFLAGS) -o $@ $<

blkdev.o: blkdev.c blkdev.h ata.h ahci.h disk.h
	$(CC) $(CFLAGS) -o $@ $<

ata.o: ata.c ata.h pci.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

ahci.o: ahci.c ahci.h ata.h mm.h paging.h pci.h process.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fatdetect.o: fatdetect.c fatdetect.h fat.h blkdev.h
	$(CC) $(CFLAGS) -o $@ $<

fatformat.o: fatformat.c fatformat.h blkdev.h mm.h
	$(CC) $(CFLAGS) -o $@ $<

format.o: format.c blkdev.h disk.h fatformat.h io.h vga.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

mouse.o: mouse.c mouse.h cirrus.h radeon.h timer.h usbhid.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

idle.o: idle.c idle.h
	$(CC) $(CFLAGS) -o $@ $<

kexec.o: kexec.c kexec.h io.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

kexec_tramp.o: kexec_tramp.S
	$(AS) $(ASFLAGS) -o $@ $<

dbg.o: dbg.c dbg.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

vfs.o: vfs.c vfs.h fat.h fatdetect.h blkdev.h io.h
	$(CC) $(CFLAGS) -o $@ $<

boot.elf: boot.o
	$(LD) $(BOOT_LDFLAGS) -o $@ $<

vesa_stage.o: vesa_stage.S
	$(AS) $(ASFLAGS) -o $@ $<

timer.o: timer.S irqret.inc
	$(AS) $(ASFLAGS) -o $@ $<

switch.o: switch.S irqret.inc
	$(AS) $(ASFLAGS) -o $@ $<

entry.o: entry.S
	$(AS) $(ASFLAGS) -o $@ $<

process.o: process.c process.h io.h kmalloc.h mm.h serial.h idle.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

mm.o: mm.c mm.h
	$(CC) $(CFLAGS) -o $@ $<

kmalloc.o: kmalloc.c kmalloc.h mm.h
	$(CC) $(CFLAGS) -o $@ $<

paging.o: paging.c paging.h mm.h mtrr.h vga.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

elf.o: elf.c elf.h mm.h paging.h
	$(CC) $(CFLAGS) -o $@ $<

tss.o: tss.c tss.h
	$(CC) $(CFLAGS) -o $@ $<

syscall.o: syscall.c syscall.h io.h process.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

sys_fs.o: sys_fs.c sys_fs.h syscall.h fat.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

sys_info.o: sys_info.c sys_info.h syscall.h ahci.h blkdev.h mm.h mouse.h usbhid.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

syscall_stub.o: syscall_stub.S irqret.inc
	$(AS) $(ASFLAGS) -o $@ $<

kernel.elf: vesa_stage.o entry.o kernel.o timer.o switch.o process.o mm.o kmalloc.o paging.o elf.o tss.o syscall.o syscall_stub.o screen.o io.o rtc.o terminal.o wm.o reboot.o poweroff.o disk.o fat.o run.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o vga_copy.o gpu.o ati.o cirrus.o nvidia.o nv50.o radeon.o mtrr.o font_8x8.o fbtext.o clock.o colors.o display.o mouse.o uhci.o ohci.o usb.o usbhid.o lsusb.o kdbg.o blkdev.o ata.o ahci.o fatdetect.o fatformat.o format.o vfs.o sys_fs.o sys_info.o idle.o kexec.o kexec_tramp.o dbg.o lspci.o peek.o ne2000.o r8168.o netdev.o netcmd.o net.o netkexec.o netlog.o dhcp.o dns.o tcp.o curl.o hello_embed.o prog_embed.o space_embed.o ucmd_cpuinfo_embed.o ucmd_ls_embed.o ucmd_cat_embed.o ucmd_save_embed.o ucmd_rm_embed.o ucmd_mkdir_embed.o ucmd_rmdir_embed.o ucmd_meminfo_embed.o ucmd_mouseinfo_embed.o ucmd_storage_embed.o
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

# Byte offsets inside boot.bin (see boot.S). Keep decimal: dd's seek= does not
# parse 0x… literals (it silently treats them as 0) and the error is hidden by
# the 2>/dev/null below.
# 62 = kernel_sectors field (0x3E)
# 87 = low byte of the "movw $1, %si" kernel start LBA (0x57)
# 26/28/32 = BPB: heads (0x1A, word), hidden sectors (0x1C, dword),
#            total sectors 32-bit (0x20, dword)
KERNEL_SECTORS_OFFSET = 62
BOOT_SECTOR_START_OFFSET = 87
BPB_HEADS_OFFSET = 26
BPB_HIDDEN_OFFSET = 28
BPB_TOTAL_OFFSET = 32

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
	printf '\x40' | dd of=boot.bin.patched bs=1 seek=$(BOOT_SECTOR_START_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\xff\x00' | dd of=boot.bin.patched bs=1 seek=$(BPB_HEADS_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x3f\x00\x00\x00' | dd of=boot.bin.patched bs=1 seek=$(BPB_HIDDEN_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x08\xb7\x01\x00' | dd of=boot.bin.patched bs=1 seek=$(BPB_TOTAL_OFFSET) conv=notrunc 2>/dev/null; \
	cat mbr.bin > $@; \
	dd if=/dev/zero bs=512 count=62 2>/dev/null >> $@; \
	cat boot.bin.patched kernel.bin >> $@; \
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

# Ring-3 commands (see the command registry in terminal.c): built as freestanding
# ELFs at USER_BASE and embedded in the kernel image so `make dev`/`net-dev`
# deliver them too.  New command: ucmd_<name>.c + a CMD_USER row in cmds[] and a
# blob in ucmds[].
UCMDS = ucmd_cpuinfo ucmd_ls ucmd_cat ucmd_save ucmd_rm ucmd_mkdir ucmd_rmdir \
        ucmd_meminfo ucmd_mouseinfo ucmd_storage

# Keep the linked ELFs around (make would treat them as disposable
# intermediates between the pattern rules).
.PRECIOUS: $(addprefix $(UCMDS),.elf)

ucmd_%.o: ucmd_%.c ucmd_lib.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_%.elf: ucmd_%.o ucmd.ld
	$(LD) -T ucmd.ld -nostdlib -m elf_i386 -o $@ $<
	$(OBJCOPY) --strip-all $@ $@

ucmd_%_embed.o: ucmd_%.elf
	$(LD) -m elf_i386 -r -b binary -o $@ $<

space.o: space.c run_api.h
	$(CC) $(CFLAGS) -o $@ $<

space.elf: space.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

space.bin: space.elf
	$(OBJCOPY) -O binary $< $@

hello.elf: hello.o
	$(LD) -T hello.ld -nostdlib -m elf_i386 -o $@ $<
	$(OBJCOPY) --strip-all $@ $@

hello_embed.o: hello.elf
	$(LD) -m elf_i386 -r -b binary -o hello_embed.o hello.elf

# User programs ship inside the kernel image so `make dev`/`make net-dev`
# update them too (boards without a readable filesystem can still run them).
prog_embed.o: prog.bin
	$(LD) -m elf_i386 -r -b binary -o prog_embed.o prog.bin

space_embed.o: space.bin
	$(LD) -m elf_i386 -r -b binary -o space_embed.o space.bin

bootstrap.o: bootstrap.c
	$(CC) $(CFLAGS) -o $@ $<

bootstrap.elf: bootstrap.o bootstrap.ld
	$(LD) -T bootstrap.ld -nostdlib -m elf_i386 -o $@ $<

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

# Write usb.img to a USB stick; prompts before destroying anything.
#   make flash USB=/dev/sdX          FORCE=1 skips the removable-device check.
flash: usb.img
	@./flash.sh "$(USB)" "$(FORCE)"

run-floppy: floppy.img root.img
	qemu-system-i386 -M pc -device pci-ohci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device pci-ohci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

run-usb: usb.img root.img
	qemu-system-i386 -M pc -boot c -device pci-ohci -device usb-mouse -drive file=usb.img,format=raw,if=ide -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -boot c -device pci-ohci -device usb-mouse -drive file=usb.img,format=raw,if=ide -drive file=root.img,format=raw,if=floppy

# UHCI variant (machines with an Intel/VIA-style controller, and the
# regression test for the UHCI backend of the USB core).
run-uhci: floppy.img root.img
	qemu-system-i386 -M pc -device piix3-usb-uhci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device piix3-usb-uhci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

# Network variant: a PCI NE2000 (RTL8029, the driver's QEMU match) on QEMU's
# user-mode network (slirp).  DHCP/DNS come from slirp, the host is 10.0.2.2
# (e.g. `python3 -m http.server` + `curl http://10.0.2.2:8000/` in a window).
run-net: floppy.img root.img
	qemu-system-i386 -M pc -device pci-ohci -device usb-mouse -device ne2k_pci,netdev=n0 -netdev user,id=n0 -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device pci-ohci -device usb-mouse -device ne2k_pci,netdev=n0 -netdev user,id=n0 -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

# SATA/AHCI variant: an ICH9 AHCI controller with a test disk on port 0.
# The disk image lives outside the repo and is created on first use; boot
# stays on the floppy (order=a), the SATA disk shows up as sata0.
SATA_IMG ?= /tmp/opencode/nebos-sata.img
run-ahci: floppy.img root.img
	@test -f $(SATA_IMG) || truncate -s 512M $(SATA_IMG)
	qemu-system-i386 -M pc -boot order=a -device pci-ohci -device usb-mouse \
	  -drive file=floppy.img,format=raw,if=floppy \
	  -drive file=root.img,format=raw,if=floppy \
	  -device ich9-ahci,id=sata \
	  -drive file=$(SATA_IMG),format=raw,if=none,id=satadisk \
	  -device ide-hd,drive=satadisk,bus=sata.0 2>/dev/null || \
	qemu-system-x86_64 -M pc -boot order=a -device pci-ohci -device usb-mouse \
	  -drive file=floppy.img,format=raw,if=floppy \
	  -drive file=root.img,format=raw,if=floppy \
	  -device ich9-ahci,id=sata \
	  -drive file=$(SATA_IMG),format=raw,if=none,id=satadisk \
	  -device ide-hd,drive=satadisk,bus=sata.0

# Headless USB HID regression tests (QEMU + python3): cursor movement via
# injected USB mouse reports and keyboard input via injected key events,
# including hotplug.  See tools/*.py.
test-usb: floppy.img root.img
	python3 tools/qmp_cursor_test.py
	python3 tools/qmp_cursor_test.py --uhci
	python3 tools/qmp_cursor_test.py --uhci72
	python3 tools/qmp_cursor_test.py --hotplug
	python3 tools/qmp_keyboard_test.py
	python3 tools/qmp_keyboard_test.py --hotplug

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

netkexec_send: netkexec_send.c
	gcc -O2 -o $@ $<

netmonitor: netmonitor.c
	gcc -O2 -o $@ $<

bootstrap: bootstrap.bin

dev: kernel.bin kexec_send
	@entry=$$(nm kernel.elf | awk '/ T kernel_entry$$/{print $$1}'); \
	if [ -z "$$entry" ]; then echo "error: kernel_entry symbol not found"; exit 1; fi; \
	echo "Sending kernel.bin (entry 0x$$entry) via $(SERIAL_DEV) @ $(SERIAL_BAUD)..."; \
	./kexec_send $(SERIAL_DEV) $(SERIAL_BAUD) $$entry kernel.bin

# Show the board's netlog stream (kernel debug + terminal text) like `make
# monitor` does over serial.
netmon: netmonitor
	@echo "Listening for netlog on UDP $(NETLOG_PORT) (Ctrl-C to exit)."
	./netmonitor $(NETLOG_PORT)

# Push a fresh kernel over Ethernet (the board auto-detects it from its idle
# loop; no serial and no USB stick involved).
BOARD_IP ?= 192.168.178.240
NETLOG_PORT ?= 19290
NETKEXEC_PORT ?= 19288

net-dev: kernel.bin netkexec_send
	@entry=$$(nm kernel.elf | awk '/ T kernel_entry$$/{print $$1}'); \
	if [ -z "$$entry" ]; then echo "error: kernel_entry symbol not found"; exit 1; fi; \
	echo "Sending kernel.bin (entry 0x$$entry) to $(BOARD_IP):$(NETKEXEC_PORT)..."; \
	./netkexec_send $(BOARD_IP) $(NETKEXEC_PORT) 0x$$entry kernel.bin

clean:
	rm -f boot.o vesa_stage.o entry.o kernel.o timer.o switch.o process.o mm.o kmalloc.o paging.o elf.o tss.o syscall.o syscall_stub.o screen.o io.o rtc.o terminal.o wm.o hello.o reboot.o poweroff.o fat.o run.o disk.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o vga_copy.o gpu.o ati.o cirrus.o nvidia.o nv50.o radeon.o mtrr.o font_8x8.o fbtext.o clock.o colors.o display.o mouse.o uhci.o ohci.o usb.o usbhid.o lsusb.o kdbg.o blkdev.o ata.o ahci.o fatdetect.o fatformat.o format.o vfs.o sys_fs.o sys_info.o idle.o kexec.o kexec_tramp.o dbg.o lspci.o peek.o ne2000.o r8168.o netdev.o netcmd.o net.o netkexec.o netlog.o dhcp.o dns.o tcp.o curl.o mbr.o boot.elf kernel.elf mbr.elf boot.bin mbr.bin kernel.bin boot.bin.tmp boot.bin.patched image.bin floppy.img usb.img final.bin root.img prog.o prog.elf prog.bin space.o space.elf space.bin hello.elf hello_embed.o prog_embed.o space_embed.o $(addprefix $(UCMDS),.o) $(addprefix $(UCMDS),.elf) $(addsuffix _embed.o,$(UCMDS)) bootstrap.o bootstrap.elf bootstrap.bin send_serial kexec_send netkexec_send netmonitor
