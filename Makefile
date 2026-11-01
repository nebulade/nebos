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

# UEFI loader: a PE32+ x86-64 EFI application built with the mingw toolchain.
# The kernel itself stays 32-bit; the loader leaves long mode before jumping.
EFI_CC   ?= x86_64-w64-mingw32-gcc
EFI_AS   ?= x86_64-w64-mingw32-as
EFI_CFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone -fno-builtin \
             -fno-asynchronous-unwind-tables -fno-unwind-tables -O2 \
             -Wall -Wextra -I. -Iefi
EFI_LDFLAGS = -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
              -Wl,--image-base,0x0 -Wl,--dynamicbase \
              -Wl,--enable-reloc-section -Wl,--no-insert-timestamp

.PHONY: all clean run run-smp run-floppy run-usb run-uhci run-xhci run-xhci-asix run-net run-e1000 run-ahci test-usb run-serial floppy usb uefi run-uefi test-uefi flash space prog bootstrap dev net-dev cmds-dev net-cmds browser-dev gallery-dev netmon monitor test-crypto test-images test-images-asan

all: floppy.img usb.img final.bin root.img

boot.o: boot.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel.o: kernel.c acpi.h cpu.h idle.h io.h lapic.h mouse.h panic.h process.h sched.h smp.h terminal.h timer.h serial.h tss.h usbhid.h version.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

screen.o: screen.c screen.h
	$(CC) $(CFLAGS) -o $@ $<

io.o: io.c io.h fbtext.h process.h screen.h tty.h
	$(CC) $(CFLAGS) -o $@ $<

tty.o: tty.c tty.h mm.h netlog.h process.h
	$(CC) $(CFLAGS) -o $@ $<

font_8x8.o: font_8x8.c font_8x8.h
	$(CC) $(CFLAGS) -o $@ $<

font_8x16.o: font_8x16.c font_8x16.h
	$(CC) $(CFLAGS) -o $@ $<

fbtext.o: fbtext.c fbtext.h font_8x8.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

terminal.o: terminal.c terminal.h screen.h vfs.h fat.h blkdev.h io.h dbg.h kexec.h process.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

wm.o: wm.c wm.h wm_abi.h terminal.h screen.h fbtext.h idle.h io.h mouse.h process.h paging.h rtc.h timer.h version.h vga.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

sys_wm.o: sys_wm.c sys_wm.h wm_abi.h wm.h paging.h process.h
	$(CC) $(CFLAGS) -o $@ $<

hello.o: hello.c
	$(CC) $(CFLAGS) -o $@ $<

reboot.o: reboot.c
	$(CC) $(CFLAGS) -o $@ $<

poweroff.o: poweroff.c
	$(CC) $(CFLAGS) -o $@ $<

disk.o: disk.c disk.h io.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

fat.o: fat.c fat.h blkdev.h io.h
	$(CC) $(CFLAGS) -Os -o $@ $<

run.o: run.c fat.h io.h fbtext.h run_api.h timer.h vga.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

rtc.o: rtc.c rtc.h
	$(CC) $(CFLAGS) -o $@ $<

serial.o: serial.c serial.h serial_ring_buf.h pci.h idle.h sched.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

serial_ring_buf.o: serial_ring_buf.c serial_ring_buf.h
	$(CC) $(CFLAGS) -o $@ $<

serial_loop.o: serial_loop.c serial.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

bin_recv.o: bin_recv.c bin_recv.h serial.h io.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

serial_recv.o: serial_recv.c serial.h bin_recv.h io.h
	$(CC) $(CFLAGS) -o $@ $<

vga.o: vga.c vga.h bootinfo.h blit.h pci.h gpu.h ati.h process.h
	$(CC) $(CFLAGS) -o $@ $<

blit.o: blit.c blit.h vga.h gfx.h cpu.h
	$(CC) $(CFLAGS) -o $@ $<

cpu.o: cpu.c cpu.h
	$(CC) $(CFLAGS) -o $@ $<

thermal.o: thermal.c thermal.h cpu.h sched.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

it87.o: it87.c it87.h pci.h
	$(CC) $(CFLAGS) -o $@ $<

gfxbench.o: gfxbench.c blit.h cpu.h fbtext.h gfx.h io.h mm.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

vga_copy.o: vga_copy.S
	$(AS) $(ASFLAGS) -o $@ $<

gpu.o: gpu.c gpu.h pci.h ati.h cirrus.h nvidia.h
	$(CC) $(CFLAGS) -o $@ $<

ati.o: ati.c ati.h pci.h radeon.h
	$(CC) $(CFLAGS) -o $@ $<

cirrus.o: cirrus.c cirrus.h cursor_shape.h pci.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

nvidia.o: nvidia.c nvidia.h nv50.h pci.h process.h serial.h
	$(CC) $(CFLAGS) -Os -o $@ $<

nv50.o: nv50.c nv50.h nvidia.h serial.h
	$(CC) $(CFLAGS) -Os -o $@ $<

radeon.o: radeon.c radeon.h cursor_shape.h pci.h paging.h serial.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

mtrr.o: mtrr.c mtrr.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

gfx.o: gfx.c gfx.h mm.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

sys_gfx.o: sys_gfx.c sys_gfx.h gfx.h paging.h process.h wm.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

display.o: display.c io.h vga.h gpu.h ati.h nvidia.h radeon.h mouse.h fbtext.h
	$(CC) $(CFLAGS) -o $@ $<

uhci.o: uhci.c uhci.h usb_hcd.h pci.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

ohci.o: ohci.c ohci.h usb_hcd.h pci.h paging.h mm.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

xhci.o: xhci.c xhci.h usb_hcd.h pci.h paging.h mm.h serial.h timer.h spinlock.h
	$(CC) $(CFLAGS) -Os -o $@ $<

asix.o: asix.c asix.h netdev.h usb.h io.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

usb.o: usb.c usb.h usb_hcd.h io.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

usbhid.o: usbhid.c usbhid.h io.h usb.h usb_hcd.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

lsusb.o: lsusb.c io.h usb.h
	$(CC) $(CFLAGS) -Os -o $@ $<

panic.o: panic.c panic.h bootinfo.h font_8x8.h process.h serial.h timer.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

bootcon.o: bootcon.c bootcon.h bootinfo.h font_8x8.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

lspci.o: lspci.c io.h pci.h
	$(CC) $(CFLAGS) -Os -o $@ $<

peek.o: peek.c io.h
	$(CC) $(CFLAGS) -Os -o $@ $<

ne2000.o: ne2000.c ne2000.h pci.h timer.h io.h
	$(CC) $(CFLAGS) -Os -o $@ $<

r8168.o: r8168.c r8168.h netdev.h io.h mm.h paging.h pci.h process.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

e1000e.o: e1000e.c e1000e.h io.h mm.h netdev.h paging.h pci.h process.h serial.h timer.h
	$(CC) $(CFLAGS) -Os -o $@ $<

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

netdev.o: netdev.c netdev.h ne2000.h r8168.h e1000e.h
	$(CC) $(CFLAGS) -o $@ $<

net.o: net.c net.h io.h netdev.h
	$(CC) $(CFLAGS) -o $@ $<

blkdev.o: blkdev.c blkdev.h ata.h ahci.h disk.h
	$(CC) $(CFLAGS) -Os -o $@ $<

ata.o: ata.c ata.h pci.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

ahci.o: ahci.c ahci.h ata.h mm.h paging.h pci.h process.h serial.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

fatdetect.o: fatdetect.c fatdetect.h fat.h blkdev.h
	$(CC) $(CFLAGS) -Os -o $@ $<

fatformat.o: fatformat.c fatformat.h blkdev.h io.h mm.h sched.h
	$(CC) $(CFLAGS) -Os -o $@ $<

format.o: format.c blkdev.h disk.h fatformat.h io.h process.h sched.h vga.h vfs.h
	$(CC) $(CFLAGS) -Os -o $@ $<

mouse.o: mouse.c mouse.h bootinfo.h cirrus.h cursor_shape.h radeon.h timer.h usbhid.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

idle.o: idle.c idle.h
	$(CC) $(CFLAGS) -o $@ $<

kexec.o: kexec.c kexec.h io.h serial.h smp.h
	$(CC) $(CFLAGS) -o $@ $<

kexec_tramp.o: kexec_tramp.S
	$(AS) $(ASFLAGS) -o $@ $<

dbg.o: dbg.c dbg.h serial.h
	$(CC) $(CFLAGS) -Os -o $@ $<

vfs.o: vfs.c vfs.h fat.h fatdetect.h blkdev.h io.h
	$(CC) $(CFLAGS) -Os -o $@ $<

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

sched.o: sched.c sched.h process.h paging.h smp.h spinlock.h sys_fs.h thermal.h timer.h tss.h vga.h
	$(CC) $(CFLAGS) -o $@ $<

acpi.o: acpi.c acpi.h bootinfo.h paging.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

lapic.o: lapic.c lapic.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

smp.o: smp.c smp.h acpi.h idle.h lapic.h paging.h process.h sched.h serial.h timer.h tss.h
	$(CC) $(CFLAGS) -o $@ $<

ap_tramp.o: ap_tramp.S
	$(AS) $(ASFLAGS) -o $@ $<

ap_tramp.elf: ap_tramp.o ap_tramp.ld
	$(LD) -T ap_tramp.ld -nostdlib -m elf_i386 -o $@ $<

ap_tramp.bin: ap_tramp.elf
	$(OBJCOPY) -O binary $< $@

ap_tramp_embed.o: ap_tramp.bin
	$(LD) -m elf_i386 -r -b binary -o ap_tramp_embed.o ap_tramp.bin

process.o: process.c process.h sched.h io.h kmalloc.h mm.h serial.h smp.h idle.h timer.h
	$(CC) $(CFLAGS) -o $@ $<

mm.o: mm.c mm.h
	$(CC) $(CFLAGS) -o $@ $<

kmalloc.o: kmalloc.c kmalloc.h mm.h
	$(CC) $(CFLAGS) -o $@ $<

paging.o: paging.c paging.h mm.h mtrr.h vga.h serial.h
	$(CC) $(CFLAGS) -o $@ $<

elf.o: elf.c elf.h mm.h paging.h
	$(CC) $(CFLAGS) -o $@ $<

tss.o: tss.c tss.h sched.h
	$(CC) $(CFLAGS) -o $@ $<

syscall.o: syscall.c syscall.h io.h process.h timer.h sys_wm.h wm_abi.h
	$(CC) $(CFLAGS) -o $@ $<

sys_fs.o: sys_fs.c sys_fs.h syscall.h fat.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

sys_mem.o: sys_mem.c sys_mem.h mm.h paging.h process.h
	$(CC) $(CFLAGS) -o $@ $<

inflate.o: inflate.c inflate.h
	$(CC) $(CFLAGS) -o $@ $<

sys_info.o: sys_info.c sys_info.h syscall.h ahci.h blkdev.h it87.h mm.h mouse.h thermal.h usbhid.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

sys_net.o: sys_net.c sys_net.h syscall.h dhcp.h dns.h net.h netdev.h netlog.h process.h sched.h
	$(CC) $(CFLAGS) -o $@ $<

syscall_stub.o: syscall_stub.S irqret.inc
	$(AS) $(ASFLAGS) -o $@ $<

kernel.elf: vesa_stage.o entry.o kernel.o timer.o switch.o sched.o process.o mm.o kmalloc.o paging.o elf.o tss.o syscall.o sys_wm.o syscall_stub.o screen.o io.o tty.o rtc.o terminal.o wm.o reboot.o poweroff.o disk.o fat.o run.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o blit.o cpu.o thermal.o it87.o vga_copy.o gfxbench.o gpu.o ati.o cirrus.o nvidia.o nv50.o radeon.o mtrr.o font_8x8.o fbtext.o gfx.o sys_gfx.o display.o mouse.o uhci.o ohci.o xhci.o asix.o usb.o usbhid.o lsusb.o panic.o bootcon.o blkdev.o ata.o ahci.o fatdetect.o fatformat.o format.o vfs.o sys_fs.o sys_info.o sys_mem.o sys_net.o idle.o kexec.o kexec_tramp.o dbg.o lspci.o peek.o ne2000.o r8168.o e1000e.o netdev.o net.o netkexec.o netlog.o dhcp.o dns.o tcp.o acpi.o lapic.o smp.o ap_tramp_embed.o prog_embed.o space_embed.o inflate.o cmds_pack.o ramfs.o share.o payload.o
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
# 64 = kernel_lba field (0x40, word; LBA 64 fits in the low byte)
# 26/28/32 = BPB: heads (0x1A, word), hidden sectors (0x1C, dword),
#            total sectors 32-bit (0x20, dword)
KERNEL_SECTORS_OFFSET = 62
BOOT_SECTOR_START_OFFSET = 64
BPB_HEADS_OFFSET = 26
BPB_HIDDEN_OFFSET = 28
BPB_TOTAL_OFFSET = 32

floppy.img: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin $@.boot.tmp; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=$@.boot.tmp bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat $@.boot.tmp kernel.bin > $@; \
	truncate -s 1474560 $@; \
	rm -f $@.boot.tmp

usb.img: mbr.bin boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin $@.boot.tmp; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=$@.boot.tmp bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x40' | dd of=$@.boot.tmp bs=1 seek=$(BOOT_SECTOR_START_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\xff\x00' | dd of=$@.boot.tmp bs=1 seek=$(BPB_HEADS_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x3f\x00\x00\x00' | dd of=$@.boot.tmp bs=1 seek=$(BPB_HIDDEN_OFFSET) conv=notrunc 2>/dev/null; \
	printf '\x08\xb7\x01\x00' | dd of=$@.boot.tmp bs=1 seek=$(BPB_TOTAL_OFFSET) conv=notrunc 2>/dev/null; \
	cat mbr.bin > $@; \
	dd if=/dev/zero bs=512 count=62 2>/dev/null >> $@; \
	cat $@.boot.tmp kernel.bin >> $@; \
	truncate -s 64M $@; \
	rm -f $@.boot.tmp

image.bin: floppy.img
	cp floppy.img image.bin

final.bin: boot.bin kernel.bin
	SECTORS=$$(( ($$(stat -c%s kernel.bin) + 511) / 512 )); \
	cp boot.bin $@.boot.tmp; \
	printf "$$(printf '\\x%02x\\x%02x' $$((SECTORS & 0xFF)) $$((SECTORS >> 8)))" | dd of=$@.boot.tmp bs=1 seek=$(KERNEL_SECTORS_OFFSET) conv=notrunc 2>/dev/null; \
	cat $@.boot.tmp kernel.bin > $@; \
	rm -f $@.boot.tmp

# ---- UEFI loader and dual-boot USB image -------------------------------
#
# The legacy image (usb.img) is left byte-for-byte alone.  usb-efi.img copies
# it, adds a third MBR partition (type 0xEF, starting at the old 64 MB end)
# and writes a small FAT16 ESP there.  BIOS booting still uses the active
# partition 1 at LBA 63; UEFI finds BOOTX64.EFI in the ESP.

efi/efi_entry.h: kernel.elf
	@mkdir -p efi
	@entry=$$(nm kernel.elf | awk '/ T kernel_entry$$/{print $$1}'); \
	if [ -z "$$entry" ]; then echo "efi: kernel_entry not found" >&2; exit 1; fi; \
	printf '#define KERNEL_ENTRY 0x%s\n' "$$entry" > $@

efi/main.o: efi/main.c efi/efi.h bootinfo.h vga.h efi/efi_entry.h
	$(EFI_CC) $(EFI_CFLAGS) -c -o $@ $<

efi/payload.o: efi/payload.S kernel.bin efi/tramp.bin
	$(EFI_AS) --64 -o $@ $<

efi/tramp.o: efi/tramp.S
	$(AS) --64 -o $@ $<

efi/tramp.elf: efi/tramp.o efi/tramp.ld
	$(LD) -T efi/tramp.ld -nostdlib -m elf_x86_64 -o $@ $<

efi/tramp.bin: efi/tramp.elf
	$(OBJCOPY) -O binary $< $@

BOOTX64.EFI: efi/main.o efi/payload.o
	$(EFI_CC) $(EFI_LDFLAGS) -o $@ efi/main.o efi/payload.o

ESP_LBA     = 131072
ESP_SECTORS = 16384

esp.img: BOOTX64.EFI
	truncate -s $$(( $(ESP_SECTORS) * 512 )) $@
	mkfs.fat -F 16 -s 2 -n NEBOSESP $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI

usb-efi.img: usb.img esp.img
	cp usb.img $@
	# The legacy partition is a raw kernel blob with a FAT16-looking BPB (the
	# BIOS only needs the boot sector and LBA 64).  UEFI firmware scans FAT
	# partitions and would mount it as a volume, writing FAT/root pages over
	# the kernel at LBA 64.  Clear BPB FATSize16 so the firmware rejects the
	# fake volume; boot.S and the BIOS never read that field.
	printf '\x00\x00' | dd of=$@ bs=1 seek=$$((63 * 512 + 0x16)) conv=notrunc 2>/dev/null
	printf '\x00\x28\x21\x08\xef\x2d\x24\x09\x00\x00\x02\x00\x00\x40\x00\x00' | \
		dd of=$@ bs=1 seek=478 conv=notrunc 2>/dev/null
	dd if=esp.img of=$@ bs=512 seek=$(ESP_LBA) conv=notrunc 2>/dev/null
	truncate -s $$(( ($(ESP_LBA) + $(ESP_SECTORS)) * 512 )) $@

uefi: usb-efi.img

OVMF ?= $(firstword $(wildcard /usr/share/ovmf/x64/OVMF.4m.fd /usr/share/OVMF/OVMF.4m.fd))

# root.img is deliberately not attached: a running QEMU may hold its write
# lock, and the UEFI boot test does not need a second filesystem.
run-uefi: usb-efi.img
	@test -n "$(OVMF)" || { echo "OVMF not found; set OVMF=/path/to/OVMF.4m.fd" >&2; exit 1; }
	qemu-system-x86_64 -M pc -m 256 -smp 4 -bios $(OVMF) -snapshot \
	  -drive file=usb-efi.img,format=raw,if=ide \
	  -device pci-ohci -device usb-mouse \
	  -device ne2k_pci,netdev=n0 -netdev user,id=n0 \
	  -serial file:/tmp/opencode/nebos-uefi-serial.log

test-uefi: usb-efi.img
	@test -n "$(OVMF)" || { echo "OVMF not found; set OVMF=/path/to/OVMF.4m.fd" >&2; exit 1; }
	@rm -f /tmp/opencode/nebos-uefi-serial.log
	timeout 40 qemu-system-x86_64 -M pc -m 256 -smp 4 -bios $(OVMF) -snapshot \
	  -drive file=usb-efi.img,format=raw,if=ide \
	  -device pci-ohci -device usb-mouse \
	  -device ne2k_pci,netdev=n0 -netdev user,id=n0 \
	  -display none -serial file:/tmp/opencode/nebos-uefi-serial.log \
	  -no-reboot >/dev/null 2>&1 || true
	@grep -q "booted" /tmp/opencode/nebos-uefi-serial.log && \
		echo "UEFI boot: OK (/tmp/opencode/nebos-uefi-serial.log)" || \
		{ echo "UEFI boot: FAILED"; tail -c 4000 /tmp/opencode/nebos-uefi-serial.log; exit 1; }

prog.o: prog.c run_api.h
	$(CC) $(CFLAGS) -o $@ $<

prog.elf: prog.o
	$(LD) -T user.ld -nostdlib -m elf_i386 -o $@ $<

prog.bin: prog.elf
	$(OBJCOPY) -O binary $< $@

space: space.bin

prog: prog.bin

# Ring-3 commands: freestanding ELFs at USER_BASE, packed into cmds.pack and
# mounted at /ramfs/bin (the shell's PATH default).  New command: add
# ucmd_<name>.c to UCMDS; the pack is rebuilt automatically.
UCMDS = ucmd_cpuinfo ucmd_ls ucmd_cat ucmd_save ucmd_rm ucmd_mkdir ucmd_rmdir \
        ucmd_meminfo ucmd_mouseinfo ucmd_storage ucmd_top \
        ucmd_clock ucmd_colors ucmd_edit ucmd_wm ucmd_term ucmd_spin \
        ucmd_netinfo ucmd_netip ucmd_netdns ucmd_host ucmd_dhcp \
        ucmd_netsend ucmd_netrecv ucmd_ping ucmd_pong ucmd_netlog \
        ucmd_netconsole ucmd_netregs ucmd_netloop ucmd_netreset ucmd_curl \
        ucmd_files ucmd_view ucmd_sensors ucmd_mines ucmd_gallery \
        ucmd_websrv ucmd_browser

# Keep the linked ELFs around (make would treat them as disposable
# intermediates between the pattern rules).
.PRECIOUS: $(addsuffix .elf,$(UCMDS))

ucmd_%.o: ucmd_%.c ucmd_lib.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_%.elf: ucmd_%.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $<
	$(OBJCOPY) --strip-all $@ $@

# edit draws text itself, so it links the kernel's 8x8 font into its ELF.
ucmd_edit.o: ucmd_edit.c ucmd_lib.h syscall.h font_8x16.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_edit.elf: ucmd_edit.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# The ring-3 WM draws its own chrome and terminal cells.
ucmd_wm.o: ucmd_wm.c ucmd_lib.h syscall.h wm_abi.h uwm_lib.h version.h
	$(CC) $(CFLAGS) -o $@ $<

uwm_lib.o: uwm_lib.c uwm_lib.h font_8x16.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_wm.elf: ucmd_wm.o uwm_lib.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

ucmd_term.o: ucmd_term.c ucmd_lib.h syscall.h wm_abi.h uwm_lib.h io.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_term.elf: ucmd_term.o uwm_lib.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# ngui: the userland immediate-mode GUI toolkit and its apps.
ngui.o: ngui.c ngui.h ucmd_lib.h syscall.h wm_abi.h font_8x16.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_files.o: ucmd_files.c ucmd_lib.h syscall.h wm_abi.h ngui.h img.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_files.elf: ucmd_files.o ngui.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# mines is a second ngui canvas app; it links the same toolkit.
ucmd_mines.o: ucmd_mines.c ucmd_lib.h syscall.h wm_abi.h ngui.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_mines.elf: ucmd_mines.o ngui.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# gallery: a live showcase of every ngui widget and primitive.  It is a disk
# app because the embedded command pack sits close to its low-memory wall;
# ucmd_gallery.c (in UCMDS) is the tiny PATH launcher for it.
gallery.o: gallery.c ucmd_lib.h syscall.h wm_abi.h ngui.h
	$(CC) $(CFLAGS) -o $@ $<

gallery.elf: gallery.o ngui.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# Image library: pure freestanding decoders + RGB565 compositing.  Only the
# viewer links them today; a browser can link the same objects later.
IMGLIB_OBJS = img.o img_mem.o img_blit.o img_png.o img_bmp.o img_gif.o \
              img_jpeg.o img_tga.o img_pnm.o img_ico.o img_pam.o img_gif_anim.o img_file.o

$(IMGLIB_OBJS): %.o: %.c img.h img_internal.h inflate.h ucmd_lib.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

# The integer IDCT deliberately relies on 32-bit wrap-around.
img_jpeg.o: CFLAGS += -fwrapv

ucmd_view.o: ucmd_view.c ucmd_lib.h syscall.h wm_abi.h ngui.h img.h img_file.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_view.elf: ucmd_view.o $(IMGLIB_OBJS) inflate.o ngui.o font_8x16.o ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter %.o,$^)
	$(OBJCOPY) --strip-all $@ $@

# TLS 1.3 client library (ring-3, linked into ucmd_curl; see tls.h).  The
# crypto files are pure freestanding C, so tls_test can exercise them on
# the host (make test-crypto).
TLSLIB_OBJS = tls_sha256.o tls_chacha20.o tls_x25519.o tls_rng.o tls.o

# Shared ring-3 HTTP/TLS client (curl and the browser).
uhttp.o: uhttp.c uhttp.h ucmd_lib.h tls.h
	$(CC) $(CFLAGS) -o $@ $<

ucmd_curl.elf: ucmd_curl.o uhttp.o $(TLSLIB_OBJS) ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter-out ucmd.ld,$^)
	$(OBJCOPY) --strip-all $@ $@

# browser: the real app is a disk image (BROWSER) because it links ngui, the
# image library, TLS and uhttp.  ucmd_browser.c is the tiny PATH launcher.
browser_html.o: browser_html.c browser_html.h
	$(CC) $(CFLAGS) -o $@ $<

browser.o: browser.c ucmd_lib.h syscall.h wm_abi.h ngui.h browser_html.h uhttp.h img.h inflate.h
	$(CC) $(CFLAGS) -o $@ $<

browser.elf: browser.o browser_html.o uhttp.o ngui.o font_8x16.o $(IMGLIB_OBJS) inflate.o $(TLSLIB_OBJS) ucmd.ld
	$(LD) -T ucmd.ld -z max-page-size=0x100 -nostdlib -m elf_i386 -o $@ $(filter-out ucmd.ld,$^)
	$(OBJCOPY) --strip-all $@ $@


tls_sha256.o: tls_sha256.c tls_sha256.h
	$(CC) $(CFLAGS) -o $@ $<

tls_chacha20.o: tls_chacha20.c tls_chacha20.h
	$(CC) $(CFLAGS) -o $@ $<

tls_x25519.o: tls_x25519.c tls_x25519.h
	$(CC) $(CFLAGS) -o $@ $<

tls_rng.o: tls_rng.c tls_rng.h tls_sha256.h ucmd_lib.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

tls.o: tls.c tls.h tls_sha256.h tls_chacha20.h tls_x25519.h tls_rng.h \
       ucmd_lib.h syscall.h
	$(CC) $(CFLAGS) -o $@ $<

tls_test: tls_test.c tls_sha256.c tls_chacha20.c tls_x25519.c \
	  tls_sha256.h tls_chacha20.h tls_x25519.h
	gcc -O2 -I. -o $@ tls_test.c tls_sha256.c tls_chacha20.c tls_x25519.c

test-crypto: tls_test
	./tls_test

# Host image-library tests (no kernel needed).  The corpus and expected hashes
# are committed; tools/gen_img_tests.py regenerates them.
IMGTEST_SRCS = img_test.c img.c img_mem.c img_blit.c img_png.c img_bmp.c \
               img_gif.c img_gif_anim.c img_jpeg.c img_tga.c img_pnm.c img_ico.c img_pam.c inflate.c
IMGTEST_DEPS = img.h img_internal.h inflate.h testdata/images/expected.h

img_test: $(IMGTEST_SRCS) $(IMGTEST_DEPS)
	gcc -O2 -fwrapv -Wall -Wextra -I. -o $@ $(IMGTEST_SRCS)

test-images: img_test
	./img_test

test-images-asan: $(IMGTEST_SRCS) $(IMGTEST_DEPS)
	# -fwrapv: the integer IDCT deliberately relies on 32-bit wrap-around.
	gcc -O1 -g -fwrapv -fsanitize=address,undefined -I. -o img_test_asan $(IMGTEST_SRCS)
	./img_test_asan

mkpack: mkpack.c
	gcc -O2 -o $@ $<

# Command pack: the ring-3 command ELFs plus hello, embedded in the kernel and
# mounted at /ramfs (bin/).  See mkpack.c and ramfs.h.
PACK_FILES = $(foreach f,$(UCMDS),bin/$(patsubst ucmd_%,%,$(f))=$(f).elf) \
             bin/hello=hello.elf

cmds.pack: $(addsuffix .elf,$(UCMDS)) hello.elf mkpack
	./mkpack $@ $(PACK_FILES)

# The embedded pack is gzip-compressed to stay inside the kernel's low-memory
# window; the raw cmds.pack is still what cmds-dev/net-cmds send.
cmds_pack_gz.bin: cmds.pack
	gzip -9 -n -c $< > $@

cmds_pack.o: cmds_pack_gz.bin
	$(LD) -m elf_i386 -r -b binary -o cmds_pack.o cmds_pack_gz.bin

payload.o: payload.c payload.h io.h mm.h ramfs.h serial.h vfs.h
	$(CC) $(CFLAGS) -o $@ $<

ramfs.o: ramfs.c ramfs.h serial.h inflate.h mm.h
	$(CC) $(CFLAGS) -o $@ $<

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

root.img: prog.bin space.bin browser.elf gallery.elf testdata/browser/demo.html testdata/images/rgb.bmp
	dd if=/dev/zero of=$@ bs=512 count=2880 2>/dev/null
	mkfs.fat -F 12 -n "ROOTDISK" $@
	echo "Hello from the second floppy!" | mcopy -i $@ - ::HELLO.TXT
	echo "This is a test file on drive 1." | mcopy -i $@ - ::TEST.TXT
	mcopy -i $@ prog.bin ::PROG.BIN
	mcopy -i $@ space.bin ::SPACE.BIN
	mcopy -i $@ browser.elf ::BROWSER
	mcopy -i $@ gallery.elf ::GALLERY
	mcopy -i $@ testdata/browser/demo.html ::DEMO.HTM
	mcopy -i $@ testdata/images/rgb.bmp ::PIC.BMP

run: image.bin root.img
	$(MAKE) run-floppy

# Four logical CPUs (the Atom 330 topology).  UP is still the default so a
# machine without ACPI/MADT boots exactly as before.
run-smp: floppy.img root.img
	qemu-system-i386 -M pc -smp 4 -device pci-ohci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -smp 4 -device pci-ohci -device usb-mouse -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

floppy: floppy.img

usb: usb.img

# Write the dual-boot USB image (legacy BIOS + UEFI) to a stick; prompts
# before destroying anything.
#   make flash USB=/dev/sdX          FORCE=1 skips the removable-device check.
#   make flash FLASH_IMG=usb.img     legacy-only image
# The UEFI half needs the mingw toolchain (EFI_CC); the legacy half does not.
FLASH_IMG ?= usb-efi.img
flash: $(FLASH_IMG)
	@./flash.sh "$(USB)" "$(FORCE)" "$(FLASH_IMG)"

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

# xHCI variant (Braswell/Intel PCH and other USB 3.0-only machines).
run-xhci: floppy.img root.img
	qemu-system-i386 -M pc -device qemu-xhci -device usb-mouse -device usb-kbd -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device qemu-xhci -device usb-mouse -device usb-kbd -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

# Passthrough of the real ASIX AX88772 dongle (requires the host's asix
# driver to be unbound and permission on /dev/bus/usb/...; typically run with
# sudo).  `-nic none` keeps the QEMU e1000 out of the way so the USB NIC is
# the only netdev.
ASIX_HOST ?= vendorid=0x0b95,productid=0x7720
run-xhci-asix: floppy.img root.img
	qemu-system-i386 -M pc -nic none -device qemu-xhci -device usb-host,$(ASIX_HOST) -serial file:/tmp/opencode/serial-asix.log -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -nic none -device qemu-xhci -device usb-host,$(ASIX_HOST) -serial file:/tmp/opencode/serial-asix.log -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

# Network variant: a PCI NE2000 (RTL8029, the driver's QEMU match) on QEMU's
# user-mode network (slirp).  DHCP/DNS come from slirp, the host is 10.0.2.2
# (e.g. `python3 -m http.server` + `curl http://10.0.2.2:8000/` in a window).
run-net: floppy.img root.img
	qemu-system-i386 -M pc -device pci-ohci -device usb-mouse -device ne2k_pci,netdev=n0 -netdev user,id=n0,hostfwd=tcp::8080-:8080 -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device pci-ohci -device usb-mouse -device ne2k_pci,netdev=n0 -netdev user,id=n0,hostfwd=tcp::8080-:8080 -drive file=floppy.img,format=raw,if=floppy -drive file=root.img,format=raw,if=floppy

# Intel e1000 variant: QEMU's 82540EM exercises the e1000e driver's MAC/DMA
# path.  (The X201's 82577LM uses the same registers; only its PCH PHY is
# different.)  usb-kbd is there for headless QMP `send-key` typing.
run-e1000: floppy.img root.img
	qemu-system-i386 -M pc -device pci-ohci -device usb-mouse -device usb-kbd \
	  -device e1000,netdev=n0 -netdev user,id=n0,hostfwd=tcp::8080-:8080 \
	  -drive file=floppy.img,format=raw,if=floppy \
	  -drive file=root.img,format=raw,if=floppy 2>/dev/null || \
	qemu-system-x86_64 -M pc -device pci-ohci -device usb-mouse -device usb-kbd \
	  -device e1000,netdev=n0 -netdev user,id=n0,hostfwd=tcp::8080-:8080 \
	  -drive file=floppy.img,format=raw,if=floppy \
	  -drive file=root.img,format=raw,if=floppy

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
	python3 tools/qmp_cursor_test.py --xhci
	python3 tools/qmp_keyboard_test.py
	python3 tools/qmp_keyboard_test.py --hotplug
	python3 tools/qmp_keyboard_test.py --xhci

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

# Push just the command pack (cmds.pack) to a running board: no kernel
# reload, the /ramfs contents are replaced in place.  The board must be idle
# at a prompt (the WM loop auto-detects the transfer).
cmds-dev: cmds.pack kexec_send
	@echo "Sending cmds.pack via $(SERIAL_DEV) @ $(SERIAL_BAUD)..."
	./kexec_send --payload $(SERIAL_DEV) $(SERIAL_BAUD) cmds.pack

net-cmds: cmds.pack netkexec_send
	@echo "Sending cmds.pack to $(BOARD_IP):$(NETKEXEC_PORT)..."
	./netkexec_send --payload $(BOARD_IP) $(NETKEXEC_PORT) cmds.pack

# The browser is a disk app, not part of the pack.  Upload it to the board's
# writable /share (the board must be running `websrv`; stop it before browsing
# HTTP, since the WebDAV server owns the NIC).
browser-dev: browser.elf
	@echo "Uploading browser.elf to http://$(BOARD_IP):8080/share/BROWSER..."
	curl -f -T browser.elf http://$(BOARD_IP):8080/share/BROWSER

# gallery follows the same disk-app model as the browser.
gallery-dev: gallery.elf
	@echo "Uploading gallery.elf to http://$(BOARD_IP):8080/share/GALLERY..."
	curl -f -T gallery.elf http://$(BOARD_IP):8080/share/GALLERY

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
	rm -f boot.o vesa_stage.o entry.o kernel.o timer.o switch.o sched.o process.o mm.o kmalloc.o paging.o elf.o tss.o syscall.o sys_wm.o syscall_stub.o screen.o io.o tty.o rtc.o terminal.o wm.o hello.o reboot.o poweroff.o fat.o run.o disk.o serial.o serial_ring_buf.o serial_loop.o serial_recv.o bin_recv.o vga.o blit.o cpu.o thermal.o it87.o vga_copy.o gfxbench.o gpu.o ati.o cirrus.o nvidia.o nv50.o radeon.o mtrr.o font_8x8.o font_8x16.o fbtext.o gfx.o sys_gfx.o display.o mouse.o uhci.o ohci.o xhci.o asix.o usb.o usbhid.o lsusb.o panic.o bootcon.o blkdev.o ata.o ahci.o fatdetect.o fatformat.o format.o vfs.o sys_fs.o sys_info.o sys_mem.o inflate.o idle.o kexec.o kexec_tramp.o dbg.o lspci.o peek.o ne2000.o r8168.o e1000e.o netdev.o net.o netkexec.o netlog.o dhcp.o dns.o tcp.o acpi.o lapic.o smp.o ap_tramp.o ap_tramp.elf ap_tramp.bin ap_tramp_embed.o mbr.o boot.elf kernel.elf mbr.elf boot.bin mbr.bin kernel.bin boot.bin.tmp boot.bin.patched image.bin floppy.img usb.img final.bin root.img prog.o prog.elf prog.bin space.o space.elf space.bin hello.elf hello_embed.o prog_embed.o space_embed.o cmds.pack cmds_pack_gz.bin cmds_pack.o ramfs.o share.o payload.o mkpack uwm_lib.o ngui.o img.o img_mem.o img_blit.o img_png.o img_bmp.o img_gif.o img_jpeg.o img_tga.o img_pnm.o img_ico.o img_pam.o img_gif_anim.o img_file.o browser.o browser_html.o uhttp.o browser.elf img_test img_test_asan *.boot.tmp $(addsuffix .o,$(UCMDS)) $(addsuffix .elf,$(UCMDS)) gallery.o gallery.elf $(TLSLIB_OBJS) tls_test bootstrap.o bootstrap.elf bootstrap.bin send_serial kexec_send netkexec_send netmonitor BOOTX64.EFI esp.img usb-efi.img efi/main.o efi/payload.o efi/tramp.o efi/tramp.elf efi/tramp.bin efi/efi_entry.h
