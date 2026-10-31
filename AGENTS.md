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
make run-uhci   # like `make run` but with a UHCI controller + USB mouse
make run-net    # like `make run` + a PCI NE2000 NIC on QEMU's user network (slirp; DHCP/DNS/NAT)
make run-ahci   # like `make run` + an ICH9 AHCI controller and a 512 MB SATA disk (sata0)
make test-usb   # headless USB mouse/keyboard regression tests (QEMU + python3)
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
   `0x600`, enables A20, enters protected mode, jumps to `kernel_entry`
   (`entry.S`, re-points ESP to the current stack top, then `kernel_main`).
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
| `0x1000`  | E820 memory map (`struct e820_map`, written by `vesa_stage.S`, parsed by `mm.c`); also the kexec trampoline's private stack top (grows down, below the map) |
| `0x3000`  | UHCI frame list (1024 × 4 B, 4 KB-aligned; `uhci.c`) |
| `0x4000`  | ISA DMA buffer, one sector (`disk.c`) |
| `_end`    | Kernel end (linker symbol; `kernel.ld` ASSERTs it stays below `0x7F000`) |
| (in-image) | Bootstrap/protected-mode stack: 8 KB `.stack` NOLOAD section, `__boot_stack_top`/`__boot_stack_bottom` (travels with the kernel) |
| `0x7FF00` | Program API struct (`PROGRAM_API_ADDR`, `run_api.h`) |
| `0x80000` | Legacy ring-0 program load area (`run.c`, `serial_recv.c`, `user.ld`; max 0x6000, inside the reserved low 1 MB) |
| `0x100000`| VGA shadow buffer (`VGA_SHADOW_BASE`, `vga.h`) |

Everything else is dynamic: process stacks and the kexec staging buffer come
from the frame allocator (≥ `0x900000`), and the fallback stack pool is a
static BSS array.  The kernel region itself only has one wall (`_end`
below the run area), so adding code no longer moves any address.

## Key files

- **Multitasking**: `process.c/h`, `switch.S` — ring-0 preemptive round-robin,
  dynamically-sized process table (kmalloc'd, grows on demand), 20 ms timeslice,
  `ps`/`kill`. Context switch happens in the PIT stub. Each process carries a
  `struct text_screen *screen` output target (child processes inherit it, so
  command output lands in the owning terminal). Exited processes are reaped and
  their stacks freed by the scheduler. The PIT stub also credits every tick to
  the running process (`cpu_ticks`/`start_ticks`; `process_procinfo()`, exposed
  as `SYS_PROCINFO`), which the ring-3 `top` command turns into a live CPU%.
- **Memory**: `mm.c/h` — INT 15h E820 detection (real-mode in `vesa_stage.S`,
  map at `0x1000`) + a bitmap page-frame allocator (4 KB pages). Reserves low
  1 MB and an 8 MB VGA-shadow region. `kmalloc.c/h` — a power-of-two heap
  allocator on top of the frame allocator (used for the process table). Process
  stacks (≥ `0x900000`) come from the frame allocator, with an 8-stack fallback
  pool (a static BSS array; no fixed address).
- **Paging**: `paging.c/h` — identity-maps RAM (4 MB pages via CR4.PSE) + the
  LFB (uncacheable) and enables CR0.PG/WP. A single shared page directory for
  now (no per-process address spaces yet). The kexec trampoline
  (`kexec_tramp.S`) clears CR0.PG before the copy so it can overwrite the old
  kernel's page tables safely.
- **Window manager**: `wm.c/h` — slot 0 runs `wm_run()`; fixed-size draggable
  terminal windows (64×32) with a system bar (banner + purple "+ New" button +
  clock + rainbow). Click-to-focus, drag-by-title-bar, close (×). The "+ New"
  button and first boot spawn terminals; there is no `term` command.  Content is
  repainted per damage rect (`wm_redraw_rect`: desktop fill + every intersecting
  window back-to-front, clipped by `wm_draw_window_clip`), not by full redraws —
  a text window only over its damaged cell block (`screen_damage_block`), a
  canvas window over its whole content.
- **Window graphics service**: `gfx.c/h` (RGB565 window canvases + bpp-aware
  blits), `sys_gfx.c/h` — a ring-3 command gets its window's canvas mapped RW at
  `0x50000000` (`SYS_GFX_BEGIN`; a mapped back buffer) and publishes frames with
  `SYS_GFX_PRESENT`, which copies it into the composited front buffer (double
  buffering; the copy runs with interrupts off, so a half-drawn frame can never
  reach the screen).  Presented frames carry monotonic seq counters, so the WM
  can never lose one.
  Keys of a window with a live session go to the session (`SYS_KEY_POLL`; Ctrl+C
  still reaches the terminal), closing a window kills its session, and
  `sys_gfx_reap`/`sys_gfx_collect` free the canvas after exit/crash.  `clock`
  and `colors` are its users.
- **Graphics**: `vga.c/h`, `vga_copy.S`, `blit.c/h`, `gpu.c`, `ati.c`, `nvidia.c`,
  `nv50.c`, `pci.h` — VESA LFB + shadow buffer.  Drawing goes into a
  `struct surface` render target: `vga_target()` is the screen's shadow buffer
  unless the running process set an override (`vga_set_target()`, stored in the
  PCB so a preempted draw can never leak), and the WM's pixel operations go
  through the `blit` vtable (`blit.c/h`: fill/copy/blend/canvas/present/flip,
  software only today) so an accelerator can replace single ops without
  touching the WM.  `vga_target_damage()` is a no-op while the target is not
  the screen.  The boot log prints `blit backend: <name>`.  Drawing marks
  damage rectangles (a small
  list that merges rects when they overlap or touch, so a keystroke's cells
  collapse while unrelated damage stays apart); the main loop copies them to
  VRAM on every iteration under a byte budget (`VGA_FLUSH_BUDGET_BYTES`), no
  fixed frame rate.  A rect that fits the budget is copied whole, so a window
  repaint never tears; only a bigger one (a full-screen redraw) is chunked, and
  its remaining rows continue after the newer damage.  ATI Mach64 CRTC
  fixup; NVIDIA is VBE-only (never touch the legacy VGA CRTC). `nvidia.c`
  identifies the card, maps BAR0/BAR1/BAR3 and reads the MCP7x stolen-VRAM
  geometry; `nv50.c` probes the nv50-family display engine at boot (read-only,
  serial/netmon). The ION/AT3IONT-I hardware cursor is deliberately not driven:
  the VBIOS keeps the display engine (INTR_1 bit 8, `0x6194e8` = 0x803), EVO
  DMA channels never fetch until nouveau's full takeover (which re-programs the
  mode itself), the cursor staging registers are engine-only, and a channel
  that has run cannot be restarted — see the header of `nv50.c`.
- **Text console**: `fbtext.c`, `font_8x8.c/h`, `io.c/h`, `screen.c/h` — 8×8 font,
  framebuffer glyph blitting (`fbtext_draw_cells`) and per-terminal cell buffers
  (`struct text_screen`).  A screen tracks the changed cell block
  (`screen_damage_block()`: `dmg_col0..dmg_col1`, `dmg_row0..dmg_row1`, grown by
  `screen_damage_cell()`/`screen_damage_cursor()`, reset by
  `screen_clear_damage()`), so the WM repaints a keystroke's cell instead of the
  whole window content and the blink only marks the cursor's own cell.
- **On-screen debug**: `kdbg.c/h` — framebuffer debug overlay (live status rows,
  heartbeat, boot info rows, exception banner) for machines without serial; see
  the kdbg section below.
- **Shell**: `terminal.c` — history, line editing, and a table-driven command
  registry (`cmds[]`: name, usage, spawn kind/handler) that holds only the
  builtins (`cd`, `path`, `help`, `clear`) and the kernel commands.  Everything
  else is a file on the per-terminal **PATH** (`struct term.path`, default
  `/ramfs/bin`, `path` builtin to show/set): dispatch is builtins -> PATH ->
  kernel commands, so a packed command can shadow a kernel one.  Tab
  completion and `help` list the registry plus the PATH contents.  Runs in a
  window (`term_run(struct term *)`); reads keys from its per-terminal queue
  (filled by the WM).
- **Filesystem**: `fat.c` (FAT12/16/32 read/write, device-parameterized),
  `vfs.c` (mounts every FAT device under `/`), `fatdetect.c`/`fatformat.c`,
  `blkdev.c` (floppy + IDE + SATA registry), `ata.c` (ATA PIO, either the
  legacy ports or the PCI IDE interface's native BARs — `ata_init_ports()`),
  `disk.c` (FDC + ISA DMA ch2, retries).  `vfs.c` exposes an `fs_ops` vtable
  (FAT and the ramfs implement it) so the file service and the shell never
  touch `fat_*` directly; `vfs_resolve()` normalizes the path relative to the
  mount (drops `.` components, collapses and trims slashes) so directory
  paths from Tab completion ("/ramfs/bin/") work.  `sys_fs.c` is the file
  service for ring-3 commands:
  whole-file read/write, index-based directory listing, mount listing,
  unlink/mkdir/rmdir (writes to the ramfs return a read-only error).  A FAT
  operation sets `sys_fs_busy`, which the scheduler honours (no process switch
  during it): the FAT driver has static state and the floppy still needs its
  IRQ.  So expect a brief system stall (not ticks — the PIT keeps running)
  while a command reads or writes a disk.
- **SATA/AHCI**: `ahci.c/h` — polled AHCI driver (first controller only, PCI
  class `01:06:01`, no IRQs, no NCQ): port bring-up with COMRESET, IDENTIFY
  (shared `ata_identify_parse()` with `ata.c`) and READ/WRITE DMA EXT through
  a page-allocated bounce buffer (nothing large in BSS). Disks are registered
  as `sata0`..`sataN` (named by AHCI port index) and mount like IDE disks
  (`sata0p1` for an MBR partition). >2 TB disks are clamped to 32-bit LBA;
  a disk added after boot needs a rescan (no COMRESET hotplug).
- **Serial**: `serial.c`, `serial_ring_buf.c/h`, `bin_recv.c/h`, `serial_recv.c`,
  `serial_loop.c`, `send_serial.c` — 16550 UART, IRQ-driven ring buffer, XMODEM.
- **Network**: `netdev.c/h` — NIC driver registry (`struct netdev`, first probe
  wins; drivers: `r8168.c` RTL8111/8168 (PCIe, polled, C+ mode) and `ne2000.c`
  RTL8029 (QEMU/ISA)). `net.c` — ARP + ICMP echo, IPv4/UDP send + parse helpers,
  shared ARP resolution (`net_arp_resolve`) and routing (`net_next_hop`), frame
  decode, `net_checksum()`/`net_checksum_tcp()`. `dhcp.c` — minimal DHCP client
  (DISCOVER →
  OFFER → REQUEST → ACK; runs at boot when a NIC is present, 3 s budget, keeps
  the static address when no server answers; `dhcp` re-runs it; learns the DNS
  server from option 6). `netcmd.c` — the
  `netinfo`/`netrecv`/`netsend`/`netip`/`dhcp`/`pong`/`ping`/`netregs`
  commands.  `dns.c/h` — a minimal resolver (one recursive A query over UDP,
  name-compression aware; `netdns` shows/sets the server, `host <name>`
  resolves).  `tcp.c/h` — a minimal TCP *client* (connect/send/recv/close, in-order
  receive with an 8 KB receive queue so data arriving during a send is never
  lost, one segment of retransmit; the caller owns the NIC).
  `curl` — the ring-3 command: `http://` or `https://` GET with DNS, ARP,
  a dumped status/headers/body, chunked decoding (redirects are followed,
  up to 5 hops); TLS 1.3 via `tls.c/h` + `tls_sha256.c`, `tls_chacha20.c`,
  `tls_x25519.c`, `tls_rng.c`:
  `TLS_CHACHA20_POLY1305_SHA256` + X25519 only, stream-oriented record
  layer, leaf SHA-256 fingerprint printed for a manual check.  Certificates
  are **not verified** (phase A: encryption without authentication; a
  MITM is undetected) and the DRBG (`tls_rng.c`: RDTSC jitter + ticks +
  MAC) is deliberately weak.  The crypto files are pure freestanding C
  and have host vectors (`make test-crypto`, `tls_test.c`); the TLS code
  lives in the command pack, so `make net-cmds` updates it without a
  kernel reload.  Static IP default `192.168.178.240`, mask/gateway learned
  from DHCP (off-subnet traffic goes via the router).
- **Hot-reload (kexec)**: `kexec.c/h`, `kexec_tramp.S`, `entry.S`, `bootstrap.c`, and the
  host-side `kexec_send.c` — reload the kernel over serial without rebooting
  (C-light: reuses the current framebuffer, re-runs `kernel_main`).
  The transfer entry is `kernel_entry` (`entry.S`), which re-points ESP before
  any stack use: the jumping kernel's trampoline stack address comes from the
  *old* build's memory map, so a reload between different maps (e.g. after the
  kernel region grew) would otherwise land the stack inside the new image.
  `netkexec.c/h` + host-side `netkexec_send.c` do the same over UDP
  (`make net-dev BOARD_IP=…`): the WM loop auto-detects the header, the board
  acks every datagram with the next expected offset, verifies the checksum and
  jumps. While idle the same loop answers ARP/ICMP for our IP, so the board is
  reachable (and MAC-resolvable) without running any command.
- **Mouse**: `mouse.c/h` — cursor move/draw shared by the PS/2 path
  (IntelliMouse, Cirrus/Radeon hardware cursor) and the USB backend;
  `usbhid.c/h` — USB HID boot devices: find + configure a mouse and/or
  keyboard, arm their interrupt endpoints, decode mouse reports and inject
  keyboard scancodes, watch for plug/unplug.
- **USB**: `usb.c/h` — controller-agnostic core: device enumeration,
  descriptor parsing, hub recursion, bus scans, per-controller addresses, the
  hotplug watcher and the interrupt-IN consumers (mouse + keyboard).
  `usb_hcd.h` is the
  host-controller ops vtable; `uhci.c` (I/O-port UHCI, frame list at
  `0x3000`, one instance) and `ohci.c` (MMIO OHCI, up to 4 instances, HCCA +
  ED/TD lists, polling) implement it.  `lsusb.c` — the `lsusb` command (lists
  every controller and its tree).  The devices work on both controller types
  and can be plugged in at any time (root-port connect-change watcher);
  `lsusb` re-enumerates the bus, after which `usbhid` re-inits itself via a
  scan generation counter.
- **Power/idle**: `idle.c` — `mwait` C-states, `hlt` fallback.
- **User programs**: `run.c`, `run_api.h`, `prog.c`, `space.c` — raw binaries at
  `0x80000`, API function-pointer struct at `0x7FF00`.  `prog`/`space` are
  **embedded in the kernel image** (`prog_embed.o`/`space_embed.o`, like
  `hello.elf`), so `make dev`/`make net-dev` update them too and a board with
  no readable filesystem can still run them: `run <name>` and the `prog`/
  `space` commands use the embedded copy, `run /<device>/<file>` still loads
  from FAT.  Rebuilds are wired (`space.c` -> `space.bin` -> `space_embed.o` ->
  `kernel.elf`).  Note: `run`/`prog`/`space` still draw over the WM (which keeps
  repainting its desktop); the windowed path for such programs is the ring-3
  canvas service (see the window graphics bullet, used by `clock`/`colors`).
- **Ring-3 commands**: `ucmd_*.c`, `ucmd_lib.h`, `ucmd.ld`, `mkpack.c`,
  `ramfs.c/h` — freestanding ELFs linked at `USER_BASE` (`0x40000000`), packed
  by `mkpack` into `cmds.pack` (as `bin/<name>`), embedded in the kernel
  image (`cmds_pack.o`) and mounted read-only at `/ramfs`.  The shell finds
  them through PATH (`/ramfs/bin` by default), loads the whole ELF through
  the VFS and spawns it as a ring-3 process; `hello` is just `bin/hello`.
  New command: add `ucmd_<name>.c` to UCMDS — the pack, the mount and the
  PATH listing pick it up automatically.  They talk to the kernel through
  `int 0x80` (`syscall.h`: putstr/ticks/sleep/exit, the whole-file service in
  `sys_fs.c`, the state snapshots in `sys_info.c` and the network service in
  `sys_net.c`) and inherit the terminal's screen, so their output lands in
  the owning window.  Each process inherits the spawning terminal's working
  directory (`struct process.cwd`, exposed as `SYS_GETCWD`), and the file
  syscalls resolve relative paths against it, so commands keep working after
  `cd` (the shell passes arguments verbatim).  The one argument is passed as a
  C string on the top of the user stack (`process_create_user(..., arg)`), so
  `_start(const char *arg)` needs no crt0.
- **Network service**: `sys_net.c/h` — the NIC and the stack stay in the
  kernel; ring-3 commands get `SYS_NET_INFO`/config, the DNS and DHCP clients
  and the raw frame paths (`NET_TX/RX/DECODE/REPLY/LOOPBACK/REGS/RESET`), so
  the packet-level commands ported unchanged.  The receiver is owned by one
  process at a time (`netdev_acquire`): the background kexec watcher pauses
  while a command owns it, and the reaper force-releases it if that process
  dies (`sys_net_reap`, driven by `netdev_owner()`/`netdev_release_all()`).
  The TCP client is exposed as `TCP_CONNECT/PEER_MSS/SEND/RECV/CLOSE` (one
  connection at a time, `tcp.c` keeps the state) plus `NET_NEXT_HOP`/`NET_ARP`
  for the route to the peer.  Calls that wait inside the stack (ARP, DNS,
  DHCP, TCP) re-enable interrupts for the duration: the syscall stub enters
  with them off, and the deadlines spin on `get_ticks_ms()` -- a frozen tick
  counter would never satisfy them.  The netdev ownership already serializes
  net users, so being preempted mid-wait is safe.

## Shell commands

`cd [path]`, `ls [path]`, `cat <path>`, `save <path> <text>`, `rm <path>`,
`mkdir <path>`, `rmdir <path>`, `hello`, `reboot`, `poweroff`, `run <name>|<file>`, `space`,
`prog`,
`display`, `cpuinfo`, `mouseinfo`, `lsusb`, `lspci [id]`, `peek <addr> [len]`, `poke <addr> <val>`, `storage`, `meminfo`, `top`, `format <device>`,
`ps`, `kill <pid>`, `help`, `clear`, `serial test|recv`, `kexec`, `dbg`.

Network: `netinfo`, `netrecv`, `netsend`, `netip [a.b.c.d]`, `dhcp` (re-run the
DHCP client), `pong` (responds to ARP/ICMP for our IP for 30 s),
`ping <host|a.b.c.d>` (active ARP + ICMP echo with RTT; resolves DNS names,
goes via the gateway when the target is off-subnet), `netloop` (PHY loopback
TX/RX self-test), `netreset`
(full NIC re-init), `netlog <text>` (one UDP datagram to port 19290),
`netconsole on|off|host <a.b.c.d>` (the debug-output mirror, see below)
`netregs` (register dump, decoded ISR + counters), `netdns [a.b.c.d]` (show or
set the resolver) and `host <name>` (one DNS A query).  `curl <url>` fetches
`http://` or `https:// host[:port]/path` with a minimal HTTP GET and dumps the
status line, headers and body (redirects are followed, up to 5 hops; numeric
hosts skip DNS).  `https` uses the phase-A TLS 1.3 client (no certificate
verification; the leaf fingerprint is printed).  On
boot, if a NIC is present, a DHCP client runs with a 3 s budget (requesting the
current address) and falls back to the static default when no server answers.
The board also answers ARP/ICMP for its IP while idle.
`lspci` lists every PCI function (one line each); with a filter (`lspci 8168`,
`lspci 02`) it also prints BAR details, type and size for matching functions.
`peek <addr> [len]` hex-dumps memory (identity map, so MMIO registers too).
`poke <addr> <val>` writes one 32-bit dword and prints the read-back (for
driving MMIO experiments from the shell).

Each terminal has a current working directory (shown in the prompt as `[dir] > `),
changed with `cd`; paths may be absolute (`/floppy1/…`) or relative (`.`, `..`,
nested). `/` is the read-only VFS root listing the mounted filesystems (the
filesystem commands operate inside a mount, e.g. `cat /floppy1/HELLO.TXT`).

`cpuinfo`, `ls`, `cat`, `save`, `rm`, `mkdir`, `rmdir`, `meminfo`, `top`,
`clock`, `colors`, `mouseinfo`, `storage`, `netinfo`, `netip`, `netdns`, `host`, `dhcp`, `ping`,
`pong`, `netsend`, `netrecv`, `netlog`, `netconsole`, `netregs`, `netloop`,
`netreset`, `curl` and `hello` are files in `/ramfs/bin` (packed by the
Makefile), found through the shell's PATH; the rest are kernel commands
spawned as processes, or builtins (`cd`, `path`, `help`, `clear`).

## Serial hot-reload (kexec) workflow

Develop without reflashing the floppy. The serial link (COM1 ↔ `/dev/ttyUSB0`)
runs at **115200 baud**. Two paths exist:

- **Auto-detect** (the normal loop) — the WM loop watches the serial
  ring buffer for the `"KXEC"` magic and auto-spawns the reload, so `make dev`
  alone boots a fresh kernel. The manual `kexec` command is kept as a fallback
  that waits for the same image.
- **`bootstrap`** (one-time, zero-reflash) — a ring-0 program loaded through the
  existing `serial recv` + `run serial` path that switches the UART to 115200,
  receives a new kernel, and jumps into it. Used only to install the first new
  kernel onto a machine still running an old (9600 baud) build; `bootstrap.bin`
  is linked by `bootstrap.ld` at the *old* program address (`0x4A000`) because
  that is where the old build's `serial recv` writes it.

Wire protocol (host → target, sent immediately; no handshake):
`[magic "KXEC"][entry u32 LE][size u32 LE][data][checksum u8]`.
The command pack has its own frame (`make cmds-dev`), the same layout with a
`"KXNP"` magic (the entry field is ignored), which replaces /ramfs in place
(see below).

**Every iteration:**
```sh
make dev            # builds kernel.bin (which carries cmds.pack), sends
make cmds-dev       # optional: push just the command pack to a running board
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

## Command pack hot-reload (cmds-dev / net-cmds)

The commands live in `cmds.pack` (`bin/<name>` entries, built by `mkpack`),
which is embedded in `kernel.bin` and mounted read-only at `/ramfs`.  A
running board can replace it without a kernel reload:

```sh
make cmds-dev       # serial:  ./kexec_send  --payload $(SERIAL_DEV) $(SERIAL_BAUD) cmds.pack
make net-cmds       # network: ./netkexec_send --payload $(BOARD_IP) $(NETKEXEC_PORT) cmds.pack
```

The pack is received into page-allocated memory (magic "KXNP" over serial,
the same stop-and-wait protocol with a "KXNP" header over UDP), validated
*before* the swap, so a truncated or corrupt transfer leaves the running
commands untouched; a good one is installed by `ramfs_replace()` and the
previous received copy is freed.  Existing processes are unaffected (they
already loaded their ELF), and the next spawn picks up the new files.  The
WM idle loop auto-detects both the kernel (KXEC/KXNU) and the pack (KXNP)
transfers, so a plain `make cmds-dev`/`make net-cmds` is enough.

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
While a net command owns the receiver (`netrecv`/`ping`/`netloop`)
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

## USB HID (mouse + keyboard) / host controllers

`usb.c` is controller-agnostic; the drivers implement the `usb_hcd_ops`
vtable (`usb_hcd.h`): control transfers, root-port queries/reset, and two
interrupt-IN slots (0 = mouse, 1 = keyboard). `usb_hcd_init()` picks UHCI
when present, otherwise OHCI (`ohci.c` drives up to 4 instances — MCP79 has
four).

- **OHCI**: BAR0 MMIO (`paging_map_mmio()`), HCCA from the page allocator,
  static 16-byte-aligned ED/TD sets. Control transfers run on the control
  list (`HcControlHeadED` + CLF) and are polled via each TD's condition code
  — a NAKed TD stays queued and is retried by the controller. Both HID EDs
  (F = 0) are chained and referenced from all 32 HCCA interrupt-table slots,
  so their TDs are serviced every frame. No IRQs and no DoneHead handling:
  software just harvests retired TDs. **TDs must be initialized with
  CC = NotAccessed (0xF)** — OHCI has no active bit, and a zero CC makes
  software think a fresh TD already completed. TD data toggles are explicit:
  DATA0 = `0x02000000`, DATA1 = `0x03000000` (`0` means "use the ED carry
  bit"); a wrong value makes the first control transfer fail with a toggle
  mismatch on real hardware while QEMU (which ignores toggles) is happy.
  Control queues use a dummy TD as the tail anchor; the data stage is one
  TD (the controller splits it into MPS-sized packets itself).
- **UHCI**: I/O ports, frame list at `0x3000`, control QH + two more QHs
  (one per interrupt slot) serviced every frame while the controller keeps
  running. The PCI scan checks **all functions** (real PIIX3/PIIX4/VIA
  layouts put the USB function at 0:07.2 / 0:01.2) and ignores controllers
  whose BARs the firmware left unassigned. One instance only.
- **Devices** (`usbhid.c`): every HID (class 3) interface with an
  interrupt-IN endpoint gets its own interrupt slot (USB_HCD_INT_SLOTS = 6).
  Boot interfaces (subclass 1, protocol 2/1) are used as-is in boot
  protocol; other class-3 interfaces are classified by reading their HID
  report descriptor (a mouse/pointer collection or relative X/Y input is a
  mouse, a keyboard collection is a keyboard; absolute pointers/tablets are
  ignored).  Composite devices (keyboard + mouse behind one receiver) and
  several devices can therefore coexist; mouse reports from all slots are
  merged.  Mouse reports are 3 bytes (`b0&7` buttons, signed dx/dy; HID's
  +y is flipped to the PS/2 convention).  Keyboard reports are 8 bytes
  (modifier byte + 6 keycodes); they are diffed against the previous report
  and decoded through a HID-usage → scancode-set-1 table (including 0xE0
  extended keys), then injected with `io_inject_scancode()` so the
  terminal/WM see exactly what a PS/2 keyboard would produce.
  `usbhid_poll()` (from `mouse_poll_update()`) also runs the hotplug
  watcher: root-port connect changes are enumerated (throttled to 250 ms),
  `CCS` low or a few TD errors detach a device.  A full scan (`lsusb`) bumps
  `usb_generation()`, which makes `usbhid` detach and rescan, so `lsusb`
  can't leave the devices dead.  The BIOS PS/2 legacy emulation is
  deliberately disabled (PCI config `0xC0`) because the kernel now owns
  these devices.
- **Debug**: `ohci:` / `uhci:` / `usbhid:` lines go to COM1 (and through the
  netlog mirror on serial-less boards); `mouseinfo` shows the active mouse
  backend and whether a USB keyboard is attached; `usb.c`'s `USB_DEBUG` and
  `usbhid.c`'s `USBHID_DEBUG` (both default 0) add scan/report tracing.
  Headless tests: `make test-usb` runs `tools/qmp_cursor_test.py` (injects
  mouse motion with QMP `input-send-event`, verifies the cursor with
  `screendump`) and `tools/qmp_keyboard_test.py` (types with QMP `send-key`,
  verifies the screen output); both take mode flags (`--uhci`, `--uhci72`,
  `--multi`, `--hotplug`).
- **Limits**: no EHCI (high-speed devices stay invisible), hotplug watches
  root ports only (a device behind a hub is only enumerated when a scan
  runs), HID report IDs are not handled (a device that prepends report IDs
  decodes as garbage), absolute pointers/tablets are ignored, no wheel
  events, and no USB keyboard means no keyboard at all if the BIOS legacy
  emulation was the only other source (`make net-dev` still reloads the
  kernel).

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

- The default `-M pc` (QEMU ≥ 11) has **no USB controller**. The `run-floppy`
  and `run-usb` targets therefore add `-device pci-ohci -device usb-mouse`
  (the OHCI path, same controller type as the ASUS ION); `make run-uhci` runs
  the same images with `-device piix3-usb-uhci -device usb-mouse` to exercise
  the UHCI backend. To keep the controller but leave a device out (hotplug
  test), drop it and use QMP `device_add`/`device_del`. A USB keyboard can be
  added the same way (`-device usb-kbd`); QMP `send-key` then types on it
  while the QEMU window's own keys still go to the PS/2 keyboard, so the dev
  loop stays usable. Cross-check the guest's view of the tree with the
  monitor `info usb`.
- `pkill -f qemu-system-i386` matches *your own shell command line* (it contains
  the string) and kills your script; use `pkill qemu` (process-name match) or
  kill by PID.
- When a harness drives a `-serial unix:...` chardev, connect **before** the
  guest boots (immediately after QEMU starts): QEMU discards guest output
  written while no client is attached, so a late-connecting reader misses the
  boot log (kexec tests that wait for it then mis-report a failed reload).

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
framebuffer (or `WC skipped: no free MTRR entry`). An entry that already
covers the LFB base (left by a previous kexec boot -- variable MTRRs survive
a kernel reload) is reused instead of consuming another slot. Never overwrite
BIOS-populated MTRR entries — chipset/MMIO uncacheability (and the USB-legacy
SMM) breaks otherwise.

**Exception banner**: `exc_handler()` (`kernel.c`) calls `kdbg_panic()`, which
draws a red banner with vector name, EIP and CR2 at the top of the screen
(visible without serial) before the CPU halts.  Faults from ring 3 do not halt:
the process is killed (with a short message in its terminal and on serial) and
the kernel keeps running.

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
- Info rows cost ~0.5 KB BSS; keep the kernel under the `0x7F000` boundary
  (`kernel.ld` ASSERT).

## Conventions & gotchas

- **Freestanding C** — no libc, no `malloc`, no `printf`. Use `io_putstr` /
  `io_putchar` / `io_puthex` (`io.h`). Printing is VGA-framebuffer based.
- **Paging is on but flat** — identity-mapped 4 MB pages (`paging.c`), so
  physical == virtual everywhere. Still no per-process address spaces or process
  isolation: everything runs ring-0 and shares the address space.
- **Fresh memory is not zeroed** — `mm_alloc_page`/`kmalloc` hand out whatever
  RAM contained; QEMU starts guest RAM cleared, real hardware does not, so a
  structure whose fields are read before being written must be cleared on
  allocation.  `clear_proc()` (`process.c`) exists for exactly this: a stale
  `procs[i].cr3` made the boot-time MMIO mapping (`paging_map_mmio_cr3`) fault
  on the board while QEMU was fine.  Anything above `MM_MAX_PHYS` (128 MB) is
  also *unmapped*, so a wild pointer there is an instant #PF.
- **All drawing goes into the current render target** (the shadow buffer by
  default; `vga_target()`, see the graphics bullet); the main loop flushes the
  pending damage rects to VRAM on every iteration (byte
  budget, see `VGA_FLUSH_BUDGET_BYTES`) so small updates (cursor, a keystroke)
  reach the screen within a loop instead of waiting for a fixed tick. A window
  drag flushes its rectangles immediately (`wm_drag_shadow`) so each step is
  one consistent frame. Full-screen programs call `vga_clear_rgb`.
- **FAT is read/write** — FAT12/16/32 read plus write/delete/mkdir/rmdir.
  FAT32 allocation uses an FSInfo-seeded free-cluster hint; the FSInfo
  free-count is not updated and a completely full FAT32 directory is not
  grown (new cluster allocation for directories is not implemented).
- **Keep the memory map in sync** — hardcoded addresses appear in `.S`, `.c`, and
  linker scripts; changing one without the others breaks boot/loading.
- **Ring-3 data segments** — when an IRET returns to ring 3, the CPU nulls
  DS/ES/FS/GS whose DPL is below CPL 3.  Every stub that loads 0x10 to run
  kernel code must therefore restore the user data selector (0x23) before
  returning to a ring-3 context: use `IRQ_RETURN_DS <cs_offset>`
  (`irqret.inc`), as `switch.S`, `timer.S` and `syscall_stub.S` do.  QEMU does
  not model this nulling, so a missing restore only fails on real hardware
  (symptom: ring-3 #GP on its first data access, e.g. a store through a
  pointer).

## Known issues (candidates for future tasks)

1. `serial_puthex` (`serial.c`) uses `"01234589ABCDEF"` — digits **6 and 7 are
   missing** (bug).
2. `vga.c` `draw_flush_debug_square()` — leftover debug artifact, still draws a
   color-cycling 32×32 square in the corner on every flush.
3. No per-process address spaces or process isolation (paging is identity-mapped).
4. FAT32: FSInfo free-count not updated, no directory-chain growth.
5. `disk.c` `fdc_reset()` and `fdc_init_once()` overlap/duplicate logic.
6. The ring-0 `run` area (`0x80000`) and the program API at `0x7FF00` are the
   last fixed low-memory addresses; moving `prog`/`space` to ring-3 would let
   the whole block go (see the roadmap).
7. Killing/reaping a ring-3 process frees its kernel stack and page directory
   but not the user pages mapped into it (a few pages leak per command run;
   needs a page-table walk/unmap on reap).
8. `display` (needs a GPU snapshot) and `lsusb` (a 45-byte wrapper around
   the USB tree printer) are the last non-privileged commands still living in
   the kernel, plus the privileged ones (`peek`, `poke`, `dbg`, `serial`,
   `kexec`, `reboot`, `poweroff`, `format`, `run`/`prog`/`space`, `ps`/`kill`).
9. Every ring-3 command is a full ELF whose LOAD segment starts at file
   offset 0x1000 (the linker's page alignment), so ~4 KB of each ~8 KB blob is
   padding: 25 commands carry ~100 KB of it.  Linking them with
   `-z max-page-size=0x100` would remove most of that.
