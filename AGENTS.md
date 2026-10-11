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
2. `vesa_stage.S` — real-mode VESA stage. Probes INT 15h E820 into `0x1000`,
   picks best VBE mode (≥16 bpp, LFB, fits VRAM, ≤1280px), writes mode info to
   `0x600`, enables A20, enters protected mode, jumps to `kernel_main`.
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
| `0x1000`  | E820 memory map (`struct e820_map`, written by `vesa_stage.S`, parsed by `mm.c`) |
| `0x3000`  | UHCI frame list (1024 × 4 B, 4 KB-aligned; `uhci.c`) |
| `0x4000`  | ISA DMA buffer, one sector (`disk.c`) |
| `0x40000` | Kernel upper bound (`kernel.ld` ASSERT) |
| `0x40800` | Protected-mode stack top (`vesa_stage.S`, `kexec_tramp.S`) |
| `0x40F00` | Program API struct (`PROGRAM_API_ADDR`, `run_api.h`) |
| `0x41000` | User program load address (`run.c`, `user.ld`) |
| `0x50000` | Fallback process stack pool (8 × 4 KB; primary stacks come from the frame allocator, ≥ `0x900000`, see `process.c`/`mm.c`) |
| `0x58000` | kexec staging buffer (received kernel image, up to 0x38200 B; ends below the VGA window) |
| `0x100000`| VGA shadow buffer (`VGA_SHADOW_BASE`, `vga.h`) |

## Key files

- **Multitasking**: `process.c/h`, `switch.S` — ring-0 preemptive round-robin,
  dynamically-sized process table (kmalloc'd, grows on demand), 20 ms timeslice,
  `ps`/`kill`. Context switch happens in the PIT stub. Each process carries a
  `struct text_screen *screen` output target (child processes inherit it, so
  command output lands in the owning terminal). Exited processes are reaped and
  their stacks freed by the scheduler.
- **Memory**: `mm.c/h` — INT 15h E820 detection (real-mode in `vesa_stage.S`,
  map at `0x1000`) + a bitmap page-frame allocator (4 KB pages). Reserves low
  1 MB and an 8 MB VGA-shadow region. `kmalloc.c/h` — a power-of-two heap
  allocator on top of the frame allocator (used for the process table). Process
  stacks (≥ `0x900000`) come from the frame allocator, with an 8-stack fallback
  pool at `0x50000`.
- **Paging**: `paging.c/h` — identity-maps RAM (4 MB pages via CR4.PSE) + the
  LFB (uncacheable) and enables CR0.PG/WP. A single shared page directory for
  now (no per-process address spaces yet). The kexec trampoline
  (`kexec_tramp.S`) clears CR0.PG before the copy so it can overwrite the old
  kernel's page tables safely.
- **Window manager**: `wm.c/h` — slot 0 runs `wm_run()`; fixed-size draggable
  terminal windows (64×20) with a system bar (banner + purple "+ New" button +
  clock + rainbow). Click-to-focus, drag-by-title-bar, close (×). The "+ New"
  button and first boot spawn terminals; there is no `term` command.
- **Graphics**: `vga.c/h`, `vga_copy.S`, `gpu.c`, `ati.c`, `nvidia.c`, `pci.h` —
  VESA LFB + shadow buffer, 30 Hz dirty-row flush. ATI Mach64 CRTC fixup; NVIDIA no-op.
- **Text console**: `fbtext.c`, `font_8x8.c/h`, `io.c/h`, `screen.c/h` — 8×8 font,
  framebuffer glyph blitting (`fbtext_draw_cells`) and per-terminal cell buffers
  (`struct text_screen`).
- **On-screen debug**: `kdbg.c/h` — framebuffer debug overlay (live status rows,
  heartbeat, boot info rows, exception banner) for machines without serial; see
  the kdbg section below.
- **Shell**: `terminal.c` — history, line editing, command dispatch (hand-rolled
  `is_cmd_*` parsers). Runs in a window (`term_run(struct term *)`); reads keys
  from its per-terminal queue (filled by the WM).
- **Filesystem**: `fat.c` (FAT12/16/32 read + FAT12/16 write, device-parameterized),
  `vfs.c` (mounts every FAT device under `/`), `fatdetect.c`/`fatformat.c`,
  `blkdev.c` (floppy + IDE registry), `ata.c` (ATA PIO), `disk.c` (FDC + ISA DMA ch2, retries).
- **Serial**: `serial.c`, `serial_ring_buf.c/h`, `bin_recv.c/h`, `serial_recv.c`,
  `serial_loop.c`, `send_serial.c` — 16550 UART, IRQ-driven ring buffer, XMODEM.
- **Network**: `netdev.c/h` — NIC driver registry (`struct netdev`, first probe
  wins; drivers: `r8168.c` RTL8111/8168 (PCIe, polled, C+ mode) and `ne2000.c`
  RTL8029 (QEMU/ISA)). `net.c` — ARP + ICMP echo, IPv4/UDP send + parse helpers,
  frame decode, `net_checksum()`. `dhcp.c` — minimal DHCP client (DISCOVER →
  OFFER → REQUEST → ACK; runs at boot when a NIC is present, 3 s budget, keeps
  the static address when no server answers; `dhcp` re-runs it). `netcmd.c` —
  the
  `netinfo`/`netrecv`/`netsend`/`netip`/`dhcp`/`ping`/`netping`/`netregs`
  commands.  Static IP default `192.168.178.240`, mask/gateway learned from
  DHCP (off-subnet `netping` goes via the router).
- **Hot-reload (kexec)**: `kexec.c/h`, `kexec_tramp.S`, `bootstrap.c`, and the
  host-side `kexec_send.c` — reload the kernel over serial without rebooting
  (C-light: reuses the current framebuffer, re-runs `kernel_main`).
  `netkexec.c/h` + host-side `netkexec_send.c` do the same over UDP
  (`make net-dev BOARD_IP=…`): the WM loop auto-detects the header, the board
  acks every datagram with the next expected offset, verifies the checksum and
  jumps. While idle the same loop answers ARP/ICMP for our IP, so the board is
  reachable (and MAC-resolvable) without running any command.
- **Mouse**: `mouse.c` — PS/2 + IntelliMouse, hardware cursor.
- **USB**: `uhci.c/h`, `usb.c/h`, `lsusb.c` — UHCI host controller (PCI scan,
  polling transfers, no IRQ), USB core enumeration with hub recursion, and the
  `lsusb` command.
- **Power/idle**: `idle.c` — `mwait` C-states, `hlt` fallback.
- **User programs**: `run.c`, `run_api.h`, `prog.c`, `space.c` — raw binaries at
  `0x41000`, API function-pointer struct at `0x40F00`.  `prog`/`space` are
  **embedded in the kernel image** (`prog_embed.o`/`space_embed.o`, like
  `hello.elf`), so `make dev`/`make net-dev` update them too and a board with
  no readable filesystem can still run them: `run <name>` and the `prog`/
  `space` commands use the embedded copy, `run /<device>/<file>` still loads
  from FAT.  Rebuilds are wired (`space.c` -> `space.bin` -> `space_embed.o` ->
  `kernel.elf`).  Note: full-screen programs draw over the WM, which keeps
  repainting its desktop — running them windowed/cleanly is a follow-up.

## Shell commands

`cd [path]`, `ls [path]`, `cat <path>`, `save <path> <text>`, `rm <path>`,
`mkdir <path>`, `rmdir <path>`, `hello`, `reboot`, `poweroff`, `run <name>|<file>`, `space`,
`prog`,
`clock`, `colors`, `display`, `cpuinfo`, `mouseinfo`, `lsusb`, `lspci [id]`, `peek <addr> [len]`, `storage`, `mem`, `format <device>`,
`ps`, `kill <pid>`, `help`, `clear`, `serial test|recv`, `kexec`, `dbg`.

Network: `netinfo`, `netrecv`, `netsend`, `netip [a.b.c.d]`, `dhcp` (re-run the
DHCP client), `ping` (responds to ARP/ICMP for our IP for 30 s),
`netping <a.b.c.d>` (active ARP + ICMP echo with RTT, via the gateway when the
target is off-subnet), `netloop` (PHY loopback TX/RX self-test), `netreset`
(full NIC re-init), `netlog <text>` (one UDP datagram to port 19290),
`netconsole on|off|host <a.b.c.d>` (the debug-output mirror, see below) and
`netregs` (register dump, decoded ISR + counters).  On boot, if a NIC is
present, a DHCP client runs with a 3 s budget (requesting the current address)
and falls back to the static default when no server answers.  The board also
answers ARP/ICMP for its IP while idle.
`lspci` lists every PCI function (one line each); with a filter (`lspci 8168`,
`lspci 02`) it also prints BAR details, type and size for matching functions.
`peek <addr> [len]` hex-dumps memory (identity map, so MMIO registers too).

Each terminal has a current working directory (shown in the prompt as `[dir] > `),
changed with `cd`; paths may be absolute (`/floppy1/…`) or relative (`.`, `..`,
nested). `/` is the read-only VFS root listing the mounted filesystems (the
filesystem commands operate inside a mount, e.g. `cat /floppy1/HELLO.TXT`).

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

## Network hot-reload (net-dev) workflow

For boards without a serial console (the ASUS AT3IONT-I), the same reload runs
over the network instead:

```sh
make net-dev                    # pushes kernel.bin to BOARD_IP (default 192.168.178.240)
make net-dev BOARD_IP=192.168.178.42
```

The board must be idle at a shell prompt: the WM loop polls the NIC, answers
ARP/ICMP for its IP, and auto-detects the `"KXNU"` header datagram (UDP port
19288).  The host tool (`netkexec_send.c`) then does a stop-and-wait transfer:
the board acks every datagram with the next expected offset, verifies the
32-bit checksum and jumps.  ~130 KB takes about a second on a wired LAN.
While a net command owns the receiver (`netrecv`/`ping`/`netping`/`netloop`)
or a kexec is in progress, the auto-detect pauses.

The board's address comes from DHCP when a server is present (the DHCP client
runs at boot; `netinfo` shows ip/mask/gateway and the lease), so use
`make net-dev BOARD_IP=<leased address>` — or `netip` on the board to pin one.

## Network console (`make netmon`)

The serial console is mirrored to UDP port 19290, so a board without a serial
port gets the same visibility as `make monitor`:

- everything written to COM1 (`serial_puts` — driver bring-up trails, MTRR
  dumps, kexec progress) and
- everything written to a terminal screen (`io_putstr` — shell output, command
  results, `netrecv` dumps)

is buffered in RAM as it is produced and flushed from the WM loop as
line-sized datagrams.  On the host:

```sh
make netmon                 # builds + runs netmonitor (Ctrl-C to exit)
make netmon | tee board.log
./netmonitor 19290          # different port
```

The mirror is **on** by default and broadcasts; `netconsole host <laptop-ip>`
narrows it to one host (datagrams carry the broadcast MAC, so the board needs
no ARP entry for it), `netconsole off`/`on` toggles it, and `netlog <text>`
sends an ad-hoc line.  Nothing is sent before the NIC is up, the producer
never blocks (a full buffer drops characters and reports it), and the flush
pauses while a net command owns the receiver.

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

## On-screen debug overlay (`kdbg`)

For machines **without a serial console** (e.g. the ASUS AT3IONT-I), `kdbg.c/h`
draws a debug overlay straight into the VESA LFB, bypassing the shadow buffer
and the WM. It stays on screen even when the WM or the scheduler is wedged, so
a freeze can be diagnosed by reading/photographing the screen. This is how the
"freeze on first key press" was tracked down: the counters showed that no key
event ever reached the OS (all zeros) and the heartbeat stopped, i.e. the CPU
was wedged in firmware/SMM before IRQ1 was delivered.

Layout (bottom-left, and a heartbeat square bottom-right):

```
MTRRdef 00000E06 vcnt 8            <- boot info rows (yellow), up to 14
M0 b=00000000 m=FF000800 WB
...
WC=e7 b=FD000000 s=00800000
IRQ 12  SC 9C  BY 12  OT 0  MK 0   <- live status row 1 (green)
WM 6  K 000A  TRM 6  K 000A        <- live status row 2 (blue)
```

**Live status rows** (bottom two, refreshed on key events and 1×/s):

- `IRQ` — IRQ1 entries; `SC` — last scancode read from port 0x60; `BY` —
  scancode bytes read (IRQ + polling); `OT` — dropped other/AUX bytes; `MK` —
  1 if the IRQ1 storm guard tripped (IRQ1 masked; WM falls back to polling).
- `WM n K xxxx` — keys handed by the WM to the focused terminal; `TRM n K xxxx`
  — keys consumed by `term_run`; `K` = last key value.
- **Heartbeat square** (bottom-right) toggles every 500 ticks (~1 Hz) from the
  PIT IRQ. If it stops, the *CPU* is wedged, not just the UI.

**Info rows** (yellow, above the status rows) are boot-time diagnostics, e.g.
the MTRR dump from `mtrr.c`: `MTRRdef` = default-type MSR + number of variable
slots; `M0..M7` = the BIOS's own MTRR entries as found at boot (`-` = free
slot; `b`/`m` = base/address mask; type `UC`/`WB`/`WC`/`WT`/`WP`); `WC=eN ...`
= the write-combining entry the kernel added in free slot N for the
framebuffer (or `WC skipped: no free MTRR entry`). Never overwrite
BIOS-populated MTRR entries — chipset/MMIO uncacheability (and the USB-legacy
SMM) breaks otherwise.

**Exception banner**: `exc_handler()` (`kernel.c`) calls `kdbg_panic()`, which
draws a red banner with vector name, EIP and CR2 at the top of the screen
(visible without serial) before the CPU halts.

API / wiring:

- `kdbg_init()` — cache LFB geometry; call after `vga_shadow_init()`.
- `kdbg_status()` — redraw the live rows; called from key paths and the PIT.
- `kdbg_info(row, s)` — set one of 14 info rows (row 0 = lowest, at
  `fb_h-31`). Text is **copied**, so stack buffers are fine, and it is safe to
  call before `kdbg_init()` (stored, drawn once the framebuffer is up).
- `kdbg_info_alloc()` — claim the next free info row (`-1` when full); boot
  subsystems (MTRR dump, NIC) take rows in init order without colliding.
- `kdbg_pit_tick()` — called from the PIT stub (`switch.S`); blinks the
  heartbeat and refreshes the overlay every second.
- Counters live in `kdbg.c`, bumped from `io.c` (`kbd_irq`,
  `io_poll_key_with_hw`), `wm.c` (`wm_handle_keyboard`) and `terminal.c`
  (`term_run`).

Caveats:

- Pixels are written directly to the LFB; WM full redraws overwrite them but
  the 1 Hz refresh brings them back.
- Info rows cost ~0.5 KB BSS; the kernel must stay under the `0x40000`
  boundary (`kernel.ld` ASSERT).

## Conventions & gotchas

- **Freestanding C** — no libc, no `malloc`, no `printf`. Use `io_putstr` /
  `io_putchar` / `io_puthex` (`io.h`). Printing is VGA-framebuffer based.
- **Paging is on but flat** — identity-mapped 4 MB pages (`paging.c`), so
  physical == virtual everywhere. Still no per-process address spaces or process
  isolation: everything runs ring-0 and shares the address space.
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
3. No per-process address spaces or process isolation (paging is identity-mapped).
4. Read-only FAT filesystem.
5. Implicit, hardcoded memory map (see table above).
6. `disk.c` `fdc_reset()` and `fdc_init_once()` overlap/duplicate logic.
7. `terminal.c` command dispatch is ~30 near-identical `is_cmd_*` functions;
   a table-driven parser would reduce this.
