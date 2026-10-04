# AGENTS.md — Nebs OS (`nebos`)

A from-scratch, freestanding **32-bit x86 hobby operating system** written in C and
x86 assembly. Single-author project (~7,000 SLOC, no external runtime, no libc).
Targets i586 (first-gen Pentium); runs under QEMU or on real hardware.

## Build & run

Toolchain: `gcc` (32-bit multilib), `as`/`ld`/`objcopy`, `mtools` (`mcopy`/`mkfs.fat`), `qemu`.

```sh
make all        # floppy.img usb.img final.bin root.img
make run        # boot floppy.img + root.img in QEMU (fallback: qemu-system-x86_64)
make run-usb    # boot usb.img as an IDE disk
make clean
```

- Build flags (in `Makefile`): `-m32 -march=i586 -ffreestanding -fno-pie -nostdlib
  -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -O2`.
- `root.img` is a second FAT12 floppy holding user binaries (`PROG.BIN`, `SPACE.BIN`)
  and test files; it requires `mtools` to build.
- `make send PROGRAM=prog` transfers a binary to a real machine over serial.

## Boot chain

1. `boot.S` — 512-byte boot sector (FAT16 BPB). Loads contiguous kernel sectors
   to `0x7E00` (CHS for floppy, INT 13h AH=42h LBA for hard disk), jumps there.
2. `vesa_stage.S` — real-mode VESA stage. Picks best VBE mode (≥16 bpp, LFB, fits
   VRAM, ≤1280px), writes mode info to `0x600`, enables A20, enters protected mode,
   jumps to `kernel_main`.
3. `kernel.c` — `kernel_main` zeroes BSS then inits serial → I/O → idle → timer
   (PIT 1 kHz + IDT) → mouse → processes → VGA shadow buffer → terminal.
4. `mbr.S` — MBR (USB/HDD boot); loads partition boot sector at LBA 1.

## Hardcoded memory map (fragile — keep in sync when editing)

| Address    | Purpose |
|-----------|---------|
| `0x600`   | Boot framebuffer info (`struct vga_boot_fb_info`, see `vga.h`) |
| `0x7C00`  | Boot sector load address |
| `0x7E00`  | Kernel load / VESA-stage entry (`kernel.ld`) |
| `0x800`   | kexec trampoline (relocated at runtime; below the kernel) |
| `0x900`   | kexec flat GDT + GDTR (built at runtime; below the kernel) |
| `0x3000`  | UHCI frame list (1024 × 4 B, 4 KB-aligned; `uhci.c`) |
| `0x20000` | (free — kernel upper bound used to be here, now `0x30000`) |
| `0x30000` | ISA DMA buffer (`disk.c`) + kernel upper bound (`kernel.ld` ASSERT) |
| `0x30800` | Protected-mode stack top (`vesa_stage.S`) |
| `0x30F00` | Program API struct (`PROGRAM_API_ADDR`, `run_api.h`) |
| `0x31000` | User program load address (`run.c`, `user.ld`) |
| `0x42000` | Process stack base (`PROC_STACK_BASE`, 4 KB × 8, `process.h`) |
| `0x50000` | kexec staging buffer (received kernel image, up to 0x28200 B) |
| `0x100000`| VGA shadow buffer (`VGA_SHADOW_BASE`, `vga.h`) |

## Key files

- **Multitasking**: `process.c/h`, `switch.S` — ring-0 preemptive round-robin,
  8 slots, 20 ms timeslice, `ps`/`kill`. Context switch happens in the PIT stub.
- **Graphics**: `vga.c/h`, `vga_copy.S`, `gpu.c`, `ati.c`, `nvidia.c`, `pci.h` —
  VESA LFB + shadow buffer, 30 Hz dirty-row flush. ATI Mach64 CRTC fixup; NVIDIA no-op.
- **Text console**: `fbtext.c`, `font_8x8.c/h`, `io.c/h` — framebuffer text,
  8×8 font, rainbow separator, live datetime header, blinking cursor.
- **Shell**: `terminal.c` — history, line editing, command dispatch (hand-rolled
  `is_cmd_*` parsers).
- **Filesystem**: `fat.c` (FAT12/16/32 read + FAT12/16 write, device-parameterized),
  `vfs.c` (mounts every FAT device under `/`), `fatdetect.c`/`fatformat.c`,
  `blkdev.c` (floppy + IDE registry), `ata.c` (ATA PIO), `disk.c` (FDC + ISA DMA ch2, retries).
- **Serial**: `serial.c`, `serial_ring_buf.c/h`, `bin_recv.c/h`, `serial_recv.c`,
  `serial_loop.c`, `send_serial.c` — 16550 UART, IRQ-driven ring buffer, XMODEM.
- **Hot-reload (kexec)**: `kexec.c/h`, `kexec_tramp.S`, `bootstrap.c`, and the
  host-side `kexec_send.c` — reload the kernel over serial without rebooting
  (C-light: reuses the current framebuffer, re-runs `kernel_main`).
- **Mouse**: `mouse.c` — PS/2 + IntelliMouse, hardware cursor.
- **USB**: `uhci.c/h`, `usb.c/h`, `lsusb.c` — UHCI host controller (PCI scan,
  polling transfers, no IRQ), USB core enumeration with hub recursion, and the
  `lsusb` command.
- **Power/idle**: `idle.c` — `mwait` C-states, `hlt` fallback.
- **User programs**: `run.c`, `run_api.h`, `prog.c`, `space.c` — raw binaries at
  `0x31000`, API function-pointer struct at `0x30F00`. `space` = Space Invaders.

## Shell commands

`hello`, `reboot`, `poweroff`, `ls [/dev[/dir]]`, `cat /dev/file`, `save /dev/file <text>`, `rm /dev/file`,
`mkdir /dev/dir`, `rmdir /dev/dir`, `run /dev/file`, `space`, `clock`,
`colors`, `display`, `cpuinfo`, `mouseinfo`, `lsusb`, `storage`, `format <device>`,
`ps`, `kill <pid>`, `help`, `clear`, `serial test|recv`, `kexec`, `dbg`.

## Serial hot-reload (kexec) workflow

Develop without reflashing the floppy. The serial link (COM1 ↔ `/dev/ttyUSB0`)
runs at **115200 baud**. Two paths exist:

- **Auto-detect** (the normal loop) — the terminal's idle loop watches the serial
  ring buffer for the `"KXEC"` magic and auto-spawns the reload, so `make dev`
  alone boots a fresh kernel. The manual `kexec` command is kept as a fallback
  that waits for the same image.
- **`bootstrap`** (one-time, zero-reflash) — a ring-0 program loaded through the
  existing `serial recv` + `run serial` path that switches the UART to 115200,
  receives a new kernel, and jumps into it. Used only to install the first new
  kernel onto a machine still running an old (9600 baud) build.

Wire protocol (host → target, sent immediately; no handshake):
`[magic "KXEC"][entry u32 LE][size u32 LE][data][checksum u8]`.

**Every iteration:**
```sh
make dev            # builds kernel.bin, computes kernel_main via nm, sends
```
The reload is automatic. On boot the kernel prints `Nebs OS v<version> booted`
to COM1, so a serial monitor can confirm the new build headlessly:

```sh
make monitor       # tail the serial line (picocom; Ctrl-A Ctrl-X to exit)
```
Run it in a second terminal, then `make dev` in the first — each reload prints
the `... booted` line.

**One-time install onto a 9600-baud machine (no floppy):**
```sh
make bootstrap                                   # builds bootstrap.bin
# 1) start the host sender first (it waits for 'R'):
./send_serial /dev/ttyUSB0 9600 bootstrap.bin
# 2) on the PC: `serial recv` (sends 'R'), then `run serial`
# 3) the bootstrap switches the link to 115200 and waits; send the kernel:
make dev                                         # 115200
```
After the first kexec the machine is at 115200, and `make dev` is all you need
from then on.

Notes: the kexec path rebuilds a flat GDT at `0x900` before jumping (the old GDT
lives inside the overwritten kernel region), so it's safe to edit any file,
including `vesa_stage.S`/`boot.S` layout — but the video *mode* itself is only
re-applied on a full floppy/USB boot (C-light reuses the current LFB).

## Serial debug monitor (`dbg`)

A ring-0 debugger driven over the serial link. On the PC type `dbg`, then drive
commands from the host with `make monitor` (picocom):

```
r <addr> <len>          read len bytes at addr -> hex dump
w <addr> <b0> [b1 ...]  write bytes at addr
i <port>                inb (read I/O port)
o <port> <val>          outb (write I/O port)
h                       help
q                       quit
```

The monitor runs inline (blocks the terminal), so it owns the serial ring buffer
and doesn't conflict with the kexec auto-detect. Reading arbitrary physical
memory is safe (no paging); writing can corrupt the kernel, so the usual loop is
inspect → `q` → `make dev` a fresh build.

## Headless debugging via serial (no display, no keyboard)

The terminal is framebuffer-only, so a headless QEMU gives you no easy way to
*type* commands or *see* output. The reliable workflow (used for `lsusb`) is to
temporarily mirror `io_putchar` to COM1, and to invoke the code under test
directly at boot — no keystroke injection needed:

1. **Echo the framebuffer to serial** — in `io.c` add `#include "serial.h"` and a
   `serial_putc(SERIAL_COM1, c);` at the top of `io_putchar`. Every `io_putstr`/
   `io_putchar` call (terminal prompt, echo, command output) now also lands on
   COM1. `serial_puthex`/`serial_putdec` (`serial.h`) are available for structured
   dumps inside drivers.
2. **Call the code at boot** — temporarily call the function under test from
   `kernel_main` (e.g. `usb_enumerate_and_print();` before `terminal_run()`), or
   `process_create(...)` from `terminal_run()` to exercise the process context.
   QMP `send-key`/`input-send-event` and HMP `sendkey` do **not** reliably reach
   the PS/2 keyboard in headless/VNC-without-a-client mode, so don't script
   keystrokes — script the call site instead.
3. **Run QEMU headless and read the log**:
   ```sh
   qemu-system-i386 -M pc \
     -drive file=floppy.img,format=raw,if=floppy \
     -drive file=root.img,format=raw,if=floppy \
     -serial file:/tmp/opencode/serial.log \
     -display none -vga std
   # ...then: cat -A /tmp/opencode/serial.log
   ```
   The `Nebs OS v<version> booted` line is already written to COM1 at boot, so
   serial is live before the framebuffer is up — a crash in early init still shows.
4. **Revert before committing** — remove the `io.c` echo and any boot-time call;
   `git status` should show `io.c` and `kernel.c` unchanged.

QEMU gotchas that bit once:

- The default `-M pc` (QEMU ≥ 11) has **no USB controller**. Add
  `-device piix3-usb-uhci` (plus `-device usb-mouse`/`usb-hub`/…) explicitly.
  Cross-check the guest's view of the tree with the monitor `info usb`.
- `pkill -f qemu-system-i386` matches *your own shell command line* (it contains
  the string) and kills your script; use `pkill qemu` (process-name match) or
  kill by PID.

## Conventions & gotchas

- **Freestanding C** — no libc, no `malloc`, no `printf`. Use `io_putstr` /
  `io_putchar` / `io_puthex` (`io.h`). Printing is VGA-framebuffer based.
- **No paging / no MMU / no process isolation.** Everything runs ring-0; user
  programs share the kernel's address space and privileges.
- **All drawing goes into the shadow buffer** (`vga_put_pixel_rgb` etc.); a 30 Hz
  flush copies dirty rows to VRAM. Full-screen programs call `vga_clear_rgb`.
- **FAT is read-only** — parse/read only; no write/create/delete.
- **Keep the memory map in sync** — hardcoded addresses appear in `.S`, `.c`, and
  linker scripts; changing one without the others breaks boot/loading.

## Known issues (candidates for future tasks)

1. `serial_puthex` (`serial.c`) uses `"01234589ABCDEF"` — digits **6 and 7 are
   missing** (bug).
2. `vga.c` `draw_flush_debug_square()` — leftover debug artifact, still draws a
   color-cycling 32×32 square in the corner on every flush.
3. No paging/virtual memory or process isolation.
4. Read-only FAT filesystem.
5. Implicit, hardcoded memory map (see table above).
6. `disk.c` `fdc_reset()` and `fdc_init_once()` overlap/duplicate logic.
7. `terminal.c` command dispatch is ~30 near-identical `is_cmd_*` functions;
   a table-driven parser would reduce this.
