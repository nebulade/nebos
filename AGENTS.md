# AGENTS.md — NebOS (`nebos`)

A from-scratch, freestanding **32-bit x86 hobby operating system** written in C and
x86 assembly. Single-author project (~7,000 SLOC, no external runtime; the
kernel has no libc, while ring-3 commands may link the in-tree `libc.a`).
Targets i586 (first-gen Pentium); runs under QEMU or on real hardware.

## Documentation site (`docs/`)

`docs/` is the static architecture site: hand-written HTML5, one shared
`style.css` and a small hand-written SVG favicon, with no JavaScript, no build
step and no external fonts or CDNs. Open
`docs/index.html` directly (`file://` works) or serve it with
`python3 -m http.server -d docs`.

Keep the site in the same change as the code.  Architecture changes belong in
this file **and** the matching page:

| Page | Covers |
|------|--------|
| `docs/index.html` | Overview, layer diagram, at-a-glance numbers, subsystem map |
| `docs/boot.html` | BIOS/VESA, MBR, UEFI loader, kexec, boot info, image layout |
| `docs/memory.html` | Address map, E820/bitmap allocator, paging, MTRR, user arenas |
| `docs/cpu.html` | GDT/TSS, interrupts, ACPI/SMP, CFS-lite, BKL, idle/power |
| `docs/drivers.html` | PCI, FDC/PATA/AHCI, USB core + UHCI/OHCI/EHCI/xHCI, HID, sensors |
| `docs/filesystems.html` | VFS/`fs_ops`, FAT, ramfs pack, `/share`, WebDAV |
| `docs/graphics.html` | VBE/shadow, damage, blit, WM, canvas ABI, ngui, images |
| `docs/network.html` | netdev, ARP/ICMP/UDP/DHCP/DNS, TCP, TLS, netconsole/netkexec |
| `docs/userland.html` | Ring-3 model, ELF spawn, syscalls, shell/PATH, pack, apps |
| `docs/development.html` | Toolchain, make targets, hot reload, tests, debugging |

Rules:

- A change to a boot path, hardcoded address, scheduler/ABI, driver model,
  file service, graphics/WM path, protocol, syscall number, build target or
  reload workflow must update the corresponding page in the same change.  A
  new subsystem needs a section on an existing page (or a new page linked from
  the index) and a card on `docs/index.html`.
- Update `AGENTS.md` whenever the architecture or a documented invariant
  changes; `AGENTS.md` and `docs/` are both source-of-truth maps and must not
  drift.  Each page's "Source map" lists the files it documents; keep it
  current.
- Charts and diagrams are hand-written HTML/CSS or inline SVG.  No JavaScript,
  CDN, generated asset or build step.  Every charted number must come from a
  source constant/table; cite the defining file in the caption or source map.
- Keep internal links working, the active nav item correct, the layout usable
  from a phone width to a wide desktop, and the site readable from `file://`.
- Keep version strings in the page heads and footers in sync with
  `OS_NAME`/`OS_VERSION` in `version.h`.

## Build & run

Toolchain: `gcc` (32-bit multilib), `as`/`ld`/`objcopy`, `mtools` (`mcopy`/`mkfs.fat`), `qemu`.

```sh
make all        # floppy.img usb.img final.bin root.img
make run        # boot floppy.img + root.img in QEMU (fallback: qemu-system-x86_64)
make run-smp    # same with -smp 4 (BSP + 3 APs, the Atom 330 topology)
make run-usb    # boot usb.img as an IDE disk
make run-uhci   # like `make run` but with a UHCI controller + USB mouse
make run-ehci   # EHCI + UHCI companion: high-speed storage on EHCI, HID on UHCI
make run-ehci-asix # pass through a real ASIX AX88772 dongle on EHCI
make run-net    # like `make run` + a PCI NE2000 NIC on QEMU's user network (slirp; DHCP/DNS/NAT)
make run-ahci   # like `make run` + an ICH9 AHCI controller and a 512 MB SATA disk (sata0)
make uefi       # build BOOTX64.EFI and the dual-boot usb-efi.img
make run-uefi   # boot usb-efi.img under OVMF (x86-64 UEFI; serial log in /tmp/opencode)
make test-uefi  # headless OVMF boot test (greps the serial log for the boot line)
make rootfs     # build cmds.img (the 8 MB rootfs partition image)
make nowm       # diagnostic kernel with no WM; the boot console stays visible
make dev        # serial kernel + rootfs reload (KXE2)
make kernel-dev # serial kernel-only reload (keeps the running rootfs)
make net-dev    # network kernel + rootfs reload (KXN2)
make kernel-net-dev
make flash      # write the dual-boot image (legacy BIOS + UEFI) to a USB stick
make test-usb   # headless USB mouse/keyboard regression tests (QEMU + python3)
make test-images  # host image-decoder + blit tests (ASan variant: test-images-asan)
make test-libc  # host stress test for the ring-3 malloc
make clean
```

- Build flags (in `Makefile`): `-m32 -march=i586 -ffreestanding -fno-pie -nostdlib
  -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -O2`.
- The command pack is the system rootfs: `cmds.pack` (NBPAK1), gzip-compressed
  to `cmds_pack_gz.bin`, wrapped by `mkrootfs` with a one-sector `NBROOT1`
  header into `rootfs.bin` (floppy append) and `cmds.img` (the 8 MB USB
  partition image).  The kernel itself no longer carries the pack; the boot
  path loads it into RAM and `ramfs_init` takes a private copy (`bin/<name>`
  mounted at `/ramfs`).
- `root.img` is a second FAT12 floppy holding the disk fallback copies of the
  browser/gallery (`BROWSER`, `GALLERY`) and its `DEMO.HTM`/`PIC.BMP` test
  page, plus a few test files; it requires `mtools` to build.
- UEFI needs `x86_64-w64-mingw32-gcc` (the 64-bit loader) and, for the QEMU
  targets, an OVMF firmware (`OVMF=/path/to/OVMF.4m.fd` overrides the default).
  `make all` does not build the UEFI image, so a machine without mingw still
  builds the legacy floppy/USB images normally.
- `usb.img` is 9 MB: partition 1 (type `0x06`, LBA 63, 1024 sectors) is the
  boot sector + kernel and partition 2 (type `0x7F`, LBA 1088) is the 8 MB
  rootfs image (`cmds.img`), 512 KB behind the boot sector.  The header and
  the 242 KB gzip pack therefore still live in the first 1.44 MB, which even a
  CHS-only USB BIOS can read.  The boot sector's `pack_lba` field points the
  VESA stage at partition 2.  `usb-efi.img` (17 MB) adds an 8 MB FAT16 ESP
  as partition 3 (type `0xEF`, LBA 18432); `BOOTX64.EFI` embeds the kernel +
  gz pack.
  `make flash` writes the dual image; `make flash FLASH_IMG=usb.img` writes
  the legacy-only image.  One BPB byte is cleared in the dual image (disk
  offset `0x7E16`, FATSize16): without that, UEFI firmware scans partition 1,
  sees the FAT16-looking boot sector and mounts the raw kernel blob as a
  volume, writing FAT/root pages over the kernel at LBA 64.  `boot.S` and the
  BIOS never read FATSize16.  `make flash` needs the mingw toolchain because
  it builds `usb-efi.img`.

## Boot chain

1. `boot.S` — 512-byte boot sector (FAT16 BPB). Loads contiguous kernel sectors
   to `0x7E00` (CHS for floppy, INT 13h AH=42h LBA for hard disk), jumps there.
2. `vesa_stage.S` — real-mode VESA stage. Probes INT 15h E820 into `0x1000`,
   reads the `NBROOT1` rootfs header at the boot sector's `pack_lba` field
   and stages the gzip pack in the low gap at `0x60000..0x9FBFF`
   (16 KB staging chunks at `0x3000`, `rep movsb`; no protected-mode BIOS
   call), picks the best VBE mode (≥16 bpp, LFB, fits VRAM, ≤1280px), writes
   mode info to `0x600` and the 12-dword `struct boot_info`
   (magic/checksum/rsdp/flags/pack range) to `0x500`, enables A20, enters
   protected mode, copies the pack to `PACK_HIGH_ADDR` (`0x900000`, above the
   VGA-shadow reserve) and rewrites the boot-info pack address before jumping
   to `kernel_entry` (`entry.S`, re-points ESP to the current stack top, then
   `kernel_main`).  The copy lets the kernel image's BSS/boot stack extend
   past `0x60000`.  The stage prints
   `Loading rootfs [d=<drive> lba=<hex> e=<0|1> m=<C|L>]...`, then a dot
   after the header and one per 16 KB chunk, so a BIOS hang is localised.
   Drives below 0x80 use CHS (the same path the boot sector's kernel load
   used); fixed disks use EDD LBA.
3. `kernel.c` — `kernel_main` zeroes BSS then inits serial → memory/paging →
   ACPI/MADT → I/O → power → idle → processes → timer (PIT; 1 kHz busy,
   100 Hz idle + IDT) → SMP AP bring-up → mouse → VGA shadow buffer →
   terminal.
4. `mbr.S` — MBR (USB/HDD boot); loads the active partition's boot sector
   (partition 1 at LBA 63).
5. **UEFI (`efi/`)** — an x86-64 PE32+ `BOOTX64.EFI` loads from the ESP. It
   picks a GOP mode, converts `GetMemoryMap` into the E820 map at `0x1000`,
   stages `kernel.bin` below 4 GB, fills `0x500` (boot info) and `0x600`
   (framebuffer), calls `ExitBootServices`, copies the embedded gz rootfs to
   the top of RAM below 128 MB, copies a 192-byte trampoline to `0x800`,
   leaves IA-32e mode (`CR0.PG`, `EFER.LME`, `CR4.PAE/PGE/PCIDE/SMEP/SMAP/
   LA57`), copies the kernel to `0x7E00` and jumps to `kernel_entry`.  The
   BIOS boot sector / VESA stage are not executed on this path; the legacy
   chain is separate (it loads the same gz pack from the rootfs image).

SMP is ACPI-only: RSDP → RSDT/XSDT → MADT, then `LAPIC` + `INIT-SIPI-SIPI`.
There is deliberately no Intel MP-table parser.  If ACPI, a usable MADT, the
CPUID APIC bit or the LAPIC is missing, `cpu_count` stays 1 and the machine
boots exactly like before (8259 PIC + PIT, no APIC MSR/MMIO access).  This keeps
a Pentium 1 (and a Pentium II board without ACPI) on the UP path.

## Hardcoded memory map (fragile — keep in sync when editing)

| Address    | Purpose |
|-----------|---------|
| `0x500`   | Boot info (`struct boot_info`, `bootinfo.h`, 12 dwords): magic/checksum, ACPI RSDP, EFI/RGBX flags, and the boot-time rootfs pack `addr`/`size`; written by `vesa_stage.S`, the UEFI loader or kexec |
| `0x600`   | Boot framebuffer info (`struct vga_boot_fb_info`, see `vga.h`; written by `vesa_stage.S` or the UEFI loader) |
| `0x7C00`  | Boot sector load address; also the UEFI handoff stack top |
| `0x7E00`  | Kernel load / VESA-stage entry (`kernel.ld`); the UEFI loader copies the kernel here and jumps to `kernel_entry` |
| `0x800`   | kexec and UEFI handoff trampoline (relocated at runtime; below the kernel) |
| `0x900`   | kexec/UEFI flat GDT + GDTR (built at runtime; below the kernel) |
| `0x1000`  | E820 memory map (`struct e820_map`, written by `vesa_stage.S` or the UEFI loader, parsed by `mm.c`); also the kexec trampoline's private stack top (grows down, below the map) |
| `0x3000`  | UHCI frame list (1024 × 4 B, 4 KB-aligned; `uhci.c`) |
| `0x4000`  | ISA DMA buffer, one sector (`disk.c`) |
| `0x6000`  | AP real/protected-mode trampoline (`ap_tramp.S`/`ap_tramp.ld`; SIPI vector 0x06, image ends below `0x6C00`) |
| `0x6C00`  | AP trampoline real-mode stack top (grows down) |
| `0x6F00`  | AP startup info block (CR3, stack top, C entry, CPU index) |
| `0x60000` | Legacy rootfs pack staging (`vesa_stage.S`; compressed gzip data, up to `PACK_LOW_MAX` 0x3FBFF, ending below the EBDA at `0x9FC00`).  Temporary: copied to `0x900000` in protected mode before the kernel starts, so the kernel BSS/boot stack may extend past this address |
| `_end`    | Kernel end (linker symbol; `kernel.ld` ASSERTs `__bss_start <= 0x60000` so the loaded image stays below the legacy staging gap, and `_end <= 0x90000` so the top of the staging gap and the EBDA stay untouched).  BSS and the boot stack may extend past `0x60000` |
| (in-image) | Bootstrap/protected-mode stack: 8 KB `.stack` NOLOAD section, `__boot_stack_top`/`__boot_stack_bottom` (travels with the kernel) |
| `0x900000`| Legacy boot-time rootfs pack after `vesa_stage.S` relocates it (reserved by `mm_init` until `ramfs_init` takes its copy, then freed; process stacks and kexec staging also start here) |
| `0x100000`| VGA shadow buffer (`VGA_SHADOW_BASE`, `vga.h`) |

The legacy stage uses `0x3000..0x6FFF` as a 16 KB-chunk rootfs staging
buffer before the kernel starts; the UHCI frame list (`0x3000`), ISA DMA
buffer (`0x4000`) and AP trampoline (`0x6000`) reinitialize those addresses
later.

The boot-time rootfs pack placement depends on the boot path: the legacy
stage stages it in the low gap `0x60000..0x9FBFF` and copies it to
`0x900000` before the kernel starts, the UEFI loader puts it at the top of
RAM below 128 MB, and kexec leaves it in the staging buffer
(≥ `0x900000`).  `mm_init` reserves `[pack_addr, pack_addr+pack_size)` from
boot info when it is above the low 1 MB, so the legacy pack is protected
from the frame allocator too.  `ramfs_init` copies/inflates it into fresh
pages and `mm_free_range()` releases the boot region when it is above the
VGA shadow reserve.

Everything else is dynamic: process stacks and the kexec staging buffer come
from the frame allocator (≥ `0x900000`), and the fallback stack pool is a
static BSS array.  The UEFI loader's kernel staging buffer comes from
`AllocatePages` below 4 GB and only lives until the `0x800` trampoline copies
it to `0x7E00`.  The kernel region itself only has one wall (`_end` below
`0x90000`), so adding code no longer moves any address.

## Key files

- **Multitasking**: `sched.c/h`, `process.c/h`, `tss.c/h`, `switch.S` — CFS-lite
  weighted fair scheduling (weight 1..16, 64-bit vruntime, 10 ms slice; WM 16,
  default/commands/terminals 8, kworker 4), a sorted sleep list and wait queues
  (no polling scans), stable per-PCB objects in a sparse `PROC_MAX` pointer
  table, per-CPU `struct cpu`/GDT/TSS with `%gs`-based `this_cpu()`, and a
  per-CPU `current` (no global `current_pid`).  Idle is not a process slot; a
  normal `kworker` process does the housekeeping and the zombie reaper.
  `sched_lock` (IRQ-safe spinlock) protects the global run/sleep/wait queues
  and the process-table state touched by timers; `schedule()` keeps it across
  the stack switch and `sched_finish_switch()` (called from `switch.S` after
  loading the new ESP) drops it, so another CPU can never pick a process whose
  kernel stack this CPU is still unwinding.  Excess wake-up races are handled
  by skipping a queued process that is still marked running on another CPU.
  The **big kernel lock** (`bkl_*`) is a sleeping recursive mutex: every
  ring-3 syscall and every kernel thread takes it, voluntary block/sleep
  points drop it (`sched_yield`), and long serial/network waits release it via
  `bkl_pause()`/`bkl_resume()`.  A BKL holder is not timer-preempted, so it can
  safely be in the middle of kernel data structures.  Kernel threads are pinned
  to the BSP; user processes may run on any CPU.  The idle fallback uses
  `bkl_trylock()` (never blocks); the kworker and kernel commands take the BKL
  normally and release it around their sleeps.
  APs use their LAPIC timer (vector 0x30; 1 ms busy, 10 ms idle); the BSP keeps the 8259 PIC/PIT and
  is the only global-tick source.  APs do not start taking user work until
  `smp_enable_sched()`, i.e. after device/FS setup.  LAPIC IPIs (vector 0x31)
  request a reschedule; they must be EOIed on the target LAPIC.
  `SYS_PROCINFO` exposes weight/state/CPU/ticks for the ring-3 `top` command,
  and `sched`/`renice` plus `SYS_SCHEDINFO` expose the scheduler counters.
  Ring 0 is preemptible at tick boundaries unless `preempt_count` is set (FAT
  and the shadow flush) or the BKL is held; voluntary blocks/yields go through
  the `int 0x81` gate.
- **SMP / ACPI**: `acpi.c/h` (RSDP → RSDT/XSDT → MADT; CPU/LAPIC list and
  LAPIC base; 4 MB table page mapping; malformed tables are ignored), `lapic.c/h`
  (xAPIC MMIO access, BSP/AP init, INIT/SIPI/IPI, calibrated periodic timer),
  `smp.c/h` (trampoline copy, AP stacks via `process_init_ap`, INIT-SIPI-SIPI
  with a second SIPI retry, per-CPU `smp_ap_main`, `smp_stop_aps`, lazy
  `smp_enable_sched`), `ap_tramp.S` + `ap_tramp.ld` (real → protected-mode
  trampoline at `0x6000`), and `spinlock.h` (the IRQ-safe spinlocks).
  `cpu_enable_sse()` runs on every AP: an AP starts with firmware CR4, and the
  SSE2 drawing fast paths #UD until OSFXSR/OSXMMEXCPT are set.
- **Memory**: `mm.c/h` — INT 15h E820 detection (real-mode in `vesa_stage.S`,
  map at `0x1000`) + a bitmap page-frame allocator (4 KB pages). Reserves low
  1 MB and an 8 MB VGA-shadow region. `kmalloc.c/h` — a power-of-two heap
  allocator on top of the frame allocator (used for PCBs and command context).
  Process stacks (≥ `0x900000`) come from the frame allocator, with an 8-stack
  fallback pool (a static BSS array; no fixed address).
- **UEFI boot**: `bootinfo.h` (the `0x500` block and its checksum/accessors),
  `efi/efi.h` (minimal UEFI types and table/protocol layouts), `efi/main.c`
  (GOP mode choice, `GetMemoryMap` → E820, staging below 4 GB, rootfs copy,
  boot-info/framebuffer blocks, `ExitBootServices`), `efi/tramp.S` +
  `efi/tramp.ld` (the `0x800` long-mode exit, kernel copy and jump) and
  `efi/payload.S` (embeds `kernel.bin`, `cmds_pack_gz.bin` and the trampoline
  in `BOOTX64.EFI`).  The legacy `vesa_stage.S` writes the same v2 boot info
  (with `pack_addr`/`pack_size`); `vga.c`/`panic.c` honour the RGBX flag and
  skip the VBE-only ATI CRTC reset / NVIDIA stolen-VRAM alias under EFI;
  `mouse.c` forces the software cursor; `acpi.c` prefers the passed RSDP and
  maps its page before validating it.
- **Paging**: `paging.c/h` — identity-maps RAM (4 MB pages via CR4.PSE) + the
  LFB (uncacheable) and enables CR0.PG/WP. Before the switch it clears stale
  translation controls a BIOS protected-mode call can leave behind
  (`CR4.PAE/PGE/LA57/PCIDE/SMEP/SMAP` and `EFER.LME`); the X201's legacy
  stage used to return from `INT 15h AH=87h` with `CR4` set, and the first
  `CR0.PG` write then triple-faulted.  The LFB PDE range is computed from
  the page index (a base above `0xFF000000` used to wrap `base + 16 MB` and
  map nothing), and `mm_init` reserves a low LFB range so the boot console
  cannot overwrite the page directory.  Each process gets a page directory
  cloned from the kernel identity map (kernel stacks are mapped everywhere, so
  CR3 can switch on the old stack); `paging_current_cr3()` reads CR3. The kexec
  trampoline (`kexec_tramp.S`) clears CR0.PG before the copy so it can overwrite
  the old kernel's page tables safely.
- **Window manager / window server**: `wm.c/h` is the kernel window server;
  the only WM is the ring-3 process loaded from `/ramfs/bin/wm` (`ucmd_wm.c`,
  `uwm_lib.c`).  The kernel maps the shadow and each window's RGB565 front
  canvas at `WM_CONTENT_VA_BASE`, routes input, owns the cursor, kexec
  detection and the boot-console handoff.  Every window is a canvas client;
  there is no in-kernel WM or terminal fallback.  The WM paints the desktop,
  spawns the first terminal after `wm_server_boot_ready()` (set after the
  storage mount), and composites the canvases into the shadow.  The boot
  console stays visible until the WM attaches (`wm_server_attach` disables
  it), so a missing `/ramfs/bin/wm` leaves the kernel boot log on screen and
  the supervisor retries.  `wm_server_present` flushes the damage rects;
  `wm_server_key_wait/poll` provide keys.  The WM keeps a front-to-back order
  (focused first, then descending slot); every damage rectangle is painted
  by subtracting the windows in front, so an occluded window is never
  repainted and the stack is not drawn back-to-front.  A focused-window drag
  copies its shadow block (`usurf_copy`) and repaints only the old strips the
  new rectangle does not cover.  The kernel hit-test uses the same order.
  `terminal.c` keeps the privileged kernel command
  registry for the ring-3 terminal (`SYS_SPAWN_KCMD` / `term_kcmd_spawn`),
  but the kernel terminal entry/shell and the cell-window ABI are gone.
- **Window canvas API**: `gfx.c/h` allocates the RGB565 front/back canvases;
  `sys_wm.c` is the ABI bridge.  `SYS_WIN_CREATE` gives a command its window
  canvas mapped RW at `USER_WIN_BASE` (`0x50000000`).  `SYS_WIN_PRESENT`
  optionally takes a content-relative `struct wm_win_present`; the kernel
  copies only that rectangle into the front canvas (with interrupts off, so
  a half-drawn frame can never reach the screen), unions it into
  `canvas_dmg_*`, and `SYS_WM_INFO` returns the union to the WM.  A present
  with `w`/`h` zero means the whole canvas.  The terminal tracks dirty rows
  and presents only those; `uwm_lib.c` uses row-based RGB565 conversion,
  `rep stos`/`rep movs` fills and copies, and counts composited pixels in
  `usurf_pixels`.  The WM writes the last two-second interval to
  `/share/wmstats` (`frames`, `ms`, `max`, `px`) for on-target measurement.
  `SYS_WIN_INFO`, `SYS_WIN_ACK_RESIZE`, `SYS_WIN_POINTER`, `SYS_WIN_EVENT`,
  `SYS_WIN_KEY_WAIT` and the generic `SYS_KEY_POLL` handle input.  The WM
  composites the front canvas; presented frames carry monotonic seq
  counters, so the WM can never lose one.  Closing a window kills its owner;
  the reaper frees the canvas.  `clock`, `colors`,
  `term`, `files` and the other apps are canvas clients.  The old
  `SYS_GFX_BEGIN/PRESENT/END` terminal-takeover service and the kernel
  text-canvas renderer are removed.  Pointer input: `SYS_WIN_POINTER` reports
  the content-relative position, buttons and focus, and `SYS_WIN_EVENT` queues
  press/release edges with a capture window, so a click that starts in a
  window still gets its release after the pointer leaves.  The userland
  `ngui.c/h` immediate-mode toolkit builds on those: clipped RGB565
  primitives, buttons, text input, procedural icons and scrolling panes.
  `files` (`ucmd_files.c`) is its first user; `gallery` (`gallery.c`) is the
  live showcase of every control and primitive the toolkit exports.  Buttons
  fire on the release edge and keep `active` until `ui->released`: inferring
  release from the sampled `ui->buttons` races the WM event queue and drops
  clicks.  Text fields support mouse drag-selection, double-click select-all,
  Ctrl+Left/Right word jumps, Ctrl+Delete/Ctrl+Backspace word deletion and
  Ctrl+A select-all; typing or Backspace/Delete replaces a selection.  At the
  end of the line Ctrl+Delete removes the word before the caret instead of
  doing nothing.
- **Graphics**: `vga.c/h`, `vga_copy.S`, `blit.c/h`, `gpu.c`, `ati.c`, `nvidia.c`,
  `nv50.c`, `pci.h` — VESA LFB + shadow buffer.  Drawing goes into a
  `struct surface` render target: `vga_target()` is the screen's shadow buffer
  unless the running process set an override (`vga_set_target()`, stored in the
  PCB so a preempted draw can never leak), and the WM's pixel operations go
  through the `blit` vtable (`blit.c/h`: fill/copy/blend/canvas/present/flip,
  software only today) so an accelerator can replace single ops without
  touching the WM.  `vga_target_damage()` is a no-op while the target is not
  the screen.  `cpu.c` detects SSE2 (CPUID, with the EFLAGS ID-bit probe and
  the CR4.OSFXSR/OSXMMEXCPT enable, since the kernel is -mno-sse) and the
  software fill uses an SSE2 row store when it is there -- streaming
  (`movntdq`, one sfence per fill) for rows that do not fit the cache, which
  avoids the read-for-ownership traffic that makes big fills DRAM-bound on the
  Atom.  `gfxbench` times the fill/copy/cell/canvas/flush paths and reports
  the bytes the presenter really copied (`vga_present_copied`).  The flush trims a shared
  damage list, so `vga_present_shadow()` refuses to re-enter while another
  process is mid-flush (a full-screen copy is long enough to be preempted).
  The boot log prints `blit backend: <name>`.  Drawing marks
  damage rectangles (a small
  list that merges rects when they overlap or touch, so a keystroke's cells
  collapse while unrelated damage stays apart); the main loop copies them to
  VRAM on every iteration under a byte budget (`VGA_FLUSH_BUDGET_BYTES`), no
  fixed frame rate.  A rect that fits the budget is copied whole, so a window
  repaint never tears; only a bigger one (a full-screen redraw) is chunked, and
  its remaining rows continue after the newer damage.  ATI Mach64 CRTC
  fixup; NVIDIA is VBE-only (never touch the legacy VGA CRTC).  On the ION the
  framebuffer is stolen DRAM and the VBE LFB is a BAR3 window into it; the
  BIOS leaves a 512 MB uncacheable MTRR over the PCIe apertures, and since the
  lowest-numbered matching entry wins, the aperture can never be
  write-combining (~33 MB/s of separate PCIe writes).  `nvidia_lfb_in_vram()`
  therefore locates the framebuffer in the stolen VRAM (marker through the
  LFB, page-granular scan of a cached mapping) and `vga_shadow_init()` writes
  through that alias once a WC MTRR covers the stolen range (only then -- a
  cached framebuffer would stop updating the screen).  Everything else keeps
  the UC LFB: `paging.c` maps LFB pages cacheable only when `mtrr_set_wc()`
  says it covered them.  `vga_fb_lfb_wc()` reports the effective path and
  `gfxbench` prints it. `nvidia.c`
  identifies the card, maps BAR0/BAR1/BAR3 and reads the MCP7x stolen-VRAM
  geometry; `nv50.c` probes the nv50-family display engine at boot (read-only,
  serial/netmon). The ION/AT3IONT-I hardware cursor is deliberately not driven:
  the VBIOS keeps the display engine (INTR_1 bit 8, `0x6194e8` = 0x803), EVO
  DMA channels never fetch until nouveau's full takeover (which re-programs the
  mode itself), the cursor staging registers are engine-only, and a channel
  that has run cannot be restarted — see the header of `nv50.c`.
- **Text console**: `fbtext.c`, `font_8x8.c/h`, `io.c/h`, `screen.c/h` — 8×8 font,
  framebuffer glyph blitting (`fbtext_draw_cells`) and the boot-console cell
  buffer (`screen.c`, used by `io.c`'s fallback screen and `bootcon`).  The
  ring-3 terminal renders its own rows into its canvas; the kernel window
  manager has no text-cell content path.  `screen_damage_*` remains for the
  boot console's own bookkeeping.
- **Shell**: `ucmd_term.c` is the windowed shell (rows, scrollback, Tab,
  builtins `cd`, `path`, `help`, `clear`).  `terminal.c` keeps the privileged
  kernel command registry and `term_kcmd_spawn()` used through `SYS_SPAWN_KCMD`;
  commands are files on the terminal PATH (`/ramfs/bin` by default), so a
  packed command can shadow a kernel one.  The in-kernel terminal entry point
  (`term_entry()`, `term_run()`) is gone.
- **Fatal exception screen**: `panic.c/h`, `timer.S` — on a ring-0 fault,
  dumps vector/error code, GPRs, CRs, EIP instruction bytes and stack
  return-address candidates straight into the LFB (and COM1) before halting;
  see the panic section below.
- **Filesystem**: `fat.c` (FAT12/16/32 read/write, device-parameterized),
  `vfs.c` (mounts every FAT device under `/`), `fatdetect.c`/`fatformat.c`,
  `blkdev.c` (floppy + IDE + SATA registry), `ata.c` (ATA PIO, either the
  legacy ports or the PCI IDE interface's native BARs — `ata_init_ports()`;
  multi-sector PIO waits for DRQ before every sector, since drives drop it
  between sectors, polls the command-block status rather than the alt-status
  BAR, and falls back to single-sector commands when a bridge leaves DRQ
  pending after a multi-sector transfer),
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
  while a command reads or writes a disk.  Directory listings cache the first
  64 entries of the last FAT directory (`fat_cache` in `vfs.c`), so the
  WebDAV server's index 0..N-1 sequence is one directory walk instead of N;
  any write/mkdir/rmdir/unlink invalidates it.
- **`/share`**: `share.c/h` — writable, page-backed in-memory filesystem for
  network uploads (the netbooted board may have no writable disk).  Entries
  are full relative paths (like the pack ramfs) plus explicit directory
  entries, so empty directories work; names are plain strings (up to 63
  bytes), file data comes from `mm_alloc_pages()` and is freed on
  replace/delete.  Limits: 64 entries and 16 MB, volatile across reboot/kexec.
  Mounted at `/share` by `vfs_mount_all()`, so it appears in `ls /` and the
  `files` GUI.  `SYS_FS_STAT` exposes size/dir/read-only, and `sys_dirent`
  carries a display name (64 bytes) plus `raw_83` so FAT's space-padded
  fields and the name-string filesystems share one ABI.
- **SATA/AHCI**: `ahci.c/h` — polled AHCI driver (first controller only, PCI
  class `01:06:01`, no IRQs, no NCQ): port bring-up with COMRESET, IDENTIFY
  (shared `ata_identify_parse()` with `ata.c`) and READ/WRITE DMA EXT through
  a page-allocated bounce buffer (nothing large in BSS). Disks are registered
  as `sata0`..`sataN` (named by AHCI port index) and mount like IDE disks
  (`sata0p1` for an MBR partition). >2 TB disks are clamped to 32-bit LBA;
  a disk added after boot needs a rescan (no COMRESET hotplug).
- **Serial**: `serial.c`, `serial_ring_buf.c/h`, `serial_loop.c` — 16550 UART
  with an IRQ-driven ring buffer and the `serial test` loopback check.
- **Network**: `netdev.c/h` — NIC driver registry (`struct netdev`, first probe
  wins; drivers: `e1000e.c` Intel e1000/PCH LAN (82540EM emulated,
  82577LM/82578/82579; polled, PCI INTx disabled before MMIO, PCH D0 and
  no BAR sizing so the ME/AMT cannot leave the BAR undecoded; skips the
  global MAC reset while the link is up, because the PCH/ME wedges on the
  CTRL_RST access), `r8168.c`
  RTL8111/8168 (PCIe, polled, C+ mode, 64-entry RX ring), `ne2000.c`
  RTL8029 (QEMU/ISA) and `asix.c` AX88772 USB Ethernet (bulk over the USB
  core, so an EHCI/xHCI dongle is usable).  `netdev [name]` is a kernel
  command that shows the active driver or switches it (`netdev asix`,
  `netdev e1000e`, ...); it calls the selected driver's probe and leaves the
  current driver untouched on failure.  `net.c` — ARP + ICMP echo, IPv4/UDP
  send + parse helpers,
  shared ARP resolution (`net_arp_resolve`) and routing (`net_next_hop`), frame
  decode, `net_checksum()`/`net_checksum_tcp()`.  `dhcp.c` — minimal DHCP client
  (DISCOVER →
  OFFER → REQUEST → ACK; runs at boot when a NIC is present, 4 s budget, keeps
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
- **Hot-reload (kexec)**: `kexec.c/h`, `kexec_tramp.S`, `entry.S`, and the
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
  `kexec_jump()` calls `smp_stop_aps()` first: every AP gets an INIT and is
  parked in the wait-for-SIPI state before the trampoline overwrites the kernel
  they were executing.  It runs with interrupts off, resets `sched_lock` (an AP
  may have been inside a scheduler critical section when INIT arrived) and then
  jumps; the new kernel starts the APs again from scratch.
- **Mouse**: `mouse.c/h` — cursor move/draw shared by the PS/2 path
  (IntelliMouse, Cirrus/Radeon hardware cursor) and the USB backend;
  `usbhid.c/h` — USB HID boot devices: find + configure a mouse and/or
  keyboard, arm their interrupt endpoints, decode mouse reports and inject
  keyboard scancodes, watch for plug/unplug.
- **USB**: `usb.c/h` — controller-agnostic core: device enumeration,
  descriptor parsing, hub recursion, bus scans, per-controller addresses, the
  hotplug watcher and the interrupt-IN consumers (mouse + keyboard).  It keeps
  a global controller table, so EHCI (high-speed) and its UHCI/OHCI
  companions (low/full-speed) are driven together; EHCI is scanned first so
  its port resets hand non-high-speed devices back to the companions.
  `usb_hcd.h` is the host-controller ops vtable; `uhci.c` (I/O-port UHCI,
  frame list at `0x3000`, one instance), `ohci.c` (MMIO OHCI, up to 4
  instances, HCCA + ED/TD lists, polling), `ehci.c` (MMIO EHCI, QH/qTD
  asynchronous schedule, up to 4 instances) and `xhci.c` (MMIO xHCI, USB2
  root ports) implement it.  `lsusb.c` — the `lsusb` command (lists every
  controller and its tree).  The devices work on every controller type and
  can be plugged in at any time (root-port connect-change watcher); `lsusb`
  re-enumerates the bus, after which `usbhid` re-inits itself via a scan
  generation counter.
- **Power/idle**: `idle.c` — `idle auto` (the default) asks `power.c` for
  the MWAIT hint: C6 (`0x20`) on the Nehalem/Westmere allowlist, HLT
  everywhere else.  `idle hlt|c1e|c3|c6` (and the `c3ap`/`c6ap` AP-only
  variants) are explicit overrides for testing; the Atom boards keep HLT
  because of the old USB-legacy SMM hang (the UHCI/OHCI `0xC0` legacy-SMI
  disable addresses it).  Arrandale hints are C1 `0x00`, C1E `0x01`, C3
  `0x10`, C6 `0x20`.  The BSP PIT and each AP's LAPIC timer run at 1 kHz
  while a process runs and drop to 100 Hz/10 ms while the CPU is idle
  (`timer_set_hz`, `lapic_timer_set_ms`); the PIT stub advances the global
  millisecond clock by `timer_tick_advance()`, and `smp_resched_wake()`
  shortens the idle period when new work arrives.
- **Frequency/package power**: `power.c/h` — Nehalem/Westmere P-states and
  C-state setup, gated on the model allowlist (`0x1A/1E/1F/25/2C/2E/2F`)
  because the MSRs must not be touched elsewhere.  `power_init()` enables
  SpeedStep (`MISC_ENABLE` bit 16) and C1E (`POWER_CTL` bit 1), raises the
  package C-state limit in `PKG_CST_CONFIG_CONTROL` to C6 only when it is
  unlocked (bit 15), reads min/max ratios from `PLATFORM_INFO`, and starts at
  the maximum non-turbo ratio.  The per-CPU tick runs a race-to-idle
  governor: 100 ms windows pick min/max/linear target ratios (short busy
  streaks jump to max immediately) and each CPU writes its own
  `IA32_PERF_CTL`.  Nehalem/Westmere take the target ratio in bits 15:8 and
  the low byte is written as zero (a preserved VID byte made the request
  silently ineffective); `IA32_PERF_STATUS` reports the current ratio in
  bits 7:0 (the X201 boot dump shows 0x12 at the 18x max non-turbo ratio).
  The `power` command prints effective MHz (`APERF`/`MPERF`), the current
  and requested ratios, per-core C3/C6 residency and package C3/C6;
  `cpufreq [auto|min|max|off|<ratio>]` overrides the governor.
- **CPU temperature**: `thermal.c/h` — Intel DTS through `IA32_THERM_STATUS`
  (MSR `0x19C`).  Each CPU samples itself once a second from its scheduler
  tick (`struct cpu.temp_c`), and `SYS_CPU_TEMP` (`sys_info.c`) exposes the
  hottest core plus TjMax to ring 3.  The model allowlist follows coretemp
  (Core/Core 2, Nehalem/Westmere including the X201's Arrandale `0x25`, and
  Bonnell) and is what keeps `rdmsr` away from CPUs that lack the MSR.
  Nehalem and later read TjMax from `IA32_TEMPERATURE_TARGET` (MSR `0x1A2`,
  bits 23:16; the X201's i5 M 520 reports 105 °C); Bonnell has no such MSR,
  so its TjMax stays table-driven (Atom 330 = 125 °C, 230 = 100, other
  Bonnell = 90, otherwise 100).  The 330's 125 °C is Linux coretemp's
  empirical value; Intel only specifies TJ = 90 °C and Tcase-max = 85.2 °C,
  so `sensors` prints the raw delta and the 90/100/125 °C scales as well as
  the top bar's `CPU 43C` before the clock (with `BAT 87%` to its left on
  ThinkPads) and `cpuinfo`'s value plus EIST state.  Platforms without DTS
  (QEMU, Pentium) hide it.
- **Board sensors**: `it87.c/h` — read-only probe of the IT87xx Super I/O
  (the AT3IONT-I exposes three temperatures and three fan tachometers), used
  by `SYS_SENSORS` and the ring-3 `sensors` command to cross-check the DTS
  and show whether the fans turn.  Nothing is written to the fan control.
  `tpec.c/h` adds the ThinkPad embedded controller of the X201
  (`SYS_TPEC_SENSORS` for temperatures/fan, `SYS_POWER_INFO` for AC/battery):
  a `_SM_` SMBIOS type 1 check gates the probe to Lenovo/IBM machines, then
  EC reads through ports 0x62/0x66 return the temperatures at EC 0x78..0x7F /
  0xC0..0xC7 (0x80 = absent, 0x00 = unimplemented; revision 3+ at EC 0xEF
  only uses the first block), the fan tachometer at 0x84 (LSB, always read
  first) / 0x85 and the 0x2F fan mode.  The power path reads 0x38/0x39
  (bit 7 present, bit 6 discharging, bit 5 charging) and 0x46 bit 4 (AC
  online), then selects the 0x81 battery information page (high nibble
  battery, low nibble page: 0 dynamic, 1 mode, 2 design, 5 manufacturer,
  6 model) and reads the 16-byte window at 0xA0..0xAF.  A pending EC output
  byte (a hotkey event, since there is no SCI handler) is drained before
  every command, and every wait is bounded; the only EC write is the 0x81
  page selector, restored when it changed.  `sensors` labels the EC
  registers and decodes the fan mode (auto / full speed / level); the ring-3
  `powerinfo` command prints AC/battery details and the WM top bar shows the
  first present battery with a valid percentage.
- **Ring-3 address space**: the ELF image and 8 KB stack live in the 2 MB
  `USER_BASE` (`0x40000000`)..`USER_STACK_TOP` (`0x40200000`) window, which is
  too small for a full-frame image.  `SYS_MEM_ALLOC`/`SYS_MEM_GROW`/`SYS_MEM_FREE`
  (`sys_mem.c/h`) map/grow/delete a single anonymous arena at
  `USER_HEAP_BASE` (`0x60000000`), capped at 32 MB; the initial allocation is
  zeroed but pages appended by `SYS_MEM_GROW` are not (decoders initialize
  what they use), which keeps large image arenas fast to map.  The reaper
  frees it with the rest of the process and `top` counts its pages.  Commands
  that link `libc.a` own this arena through `libc/malloc.c`; the raw syscalls
  and libc malloc must never be mixed in one process.  The canvas mapping at
  `USER_GFX_BASE` (`0x50000000`) and the WM mappings are separate.
- **Ring-3 libc**: `libc/include/` (stddef/stdint/stdarg/stdlib/string/ctype/
  assert/ucmd_heap), `libc/malloc.c`, `libc/string.c`, `libc/strdup.c`,
  `libc/stdlib.c`, `libc/ucmd_mem.c`, linked as `libc.a` only into commands
  that reference it.  `malloc` is a boundary-tag first-fit allocator with
  immediate coalescing: 16-byte aligned, `calloc` zeroes, `realloc` grows in
  place when the next block is free, and the heap maps 64 KB lazily then
  grows in page-rounded steps with 64 KB slack up to the 32 MB arena cap.
  `heap_reset()` (`ucmd_heap.h`) discards every allocation and unmaps the
  arena for page-scoped apps such as the browser.  There are no threads, so
  there is no lock; each process uses either this heap or the raw
  `SYS_MEM_*` calls, never both.  The archive plus
  `-ffunction-sections`/`--gc-sections` keeps unused code out of the legacy
  pack.  Host test: `make test-libc` (`libc/test_malloc.c`).
- **Ring-3 commands**: `ucmd_*.c`, `ucmd_lib.h`, `ucmd.ld`, `mkpack.c`,
  `rootfs.h`, `mkrootfs.c`, `ramfs.c/h` — static ELFs linked at
  `USER_BASE` (`0x40000000`), packed by `mkpack` into `cmds.pack` (as
  `bin/<name>`), gzipped to `cmds_pack_gz.bin` and wrapped by `mkrootfs`
  with a `NBROOT1` header.  The pack is the system rootfs: the boot path
  loads it into RAM (legacy low gap / UEFI / kexec staging) and `ramfs_init`
  takes a private page-allocated copy, mounted read-only at `/ramfs`.  The
  shell finds them through PATH (`/ramfs/bin` by default), loads the whole
  ELF through the VFS and spawns it as a ring-3 process; `hello` is just
  `bin/hello`.  New command: add `ucmd_<name>.c` to UCMDS — the pack, the
  mount and the PATH listing pick it up automatically.  `SYS_SPAWN` still
  loads ELFs from any VFS mount, so commands can also live on `/share` or a
  FAT disk (`path /share/bin`, or an absolute path).  They talk to the kernel through
  `int 0x80` (`syscall.h`: putstr/ticks/sleep/exit, the whole-file service in
  `sys_fs.c`, the state snapshots in `sys_info.c` and the network service in
  `sys_net.c`) and inherit the terminal's screen, so their output lands in
  the owning window.  Each process inherits the spawning terminal's working
  directory (`struct process.cwd`, exposed as `SYS_GETCWD`), and the file
  syscalls resolve relative paths against it, so commands keep working after
  `cd` (the shell passes arguments verbatim).  The one argument is passed as a
  C string on the top of the user stack (`process_create_user(..., arg)`), so
  `_start(const char *arg)` needs no crt0.  `ngui.c/h` is the shared userland
  GUI toolkit linked into `ucmd_files.c`, `ucmd_mines.c`, `ucmd_solitaire.c`
  and the external `gallery.elf` (buttons, menu items, text fields, icons,
  layout cuts and scroll panes); `files` opens
  the selected text file through
  `/ramfs/bin/edit`, image files through `/ramfs/bin/view`, and `.htm`/`.html`
  files through `/ramfs/bin/browser`.  `ngui_end_ex()`
  is the variant that runs the bookkeeping without publishing the canvas, for
  apps that poll input without redrawing.
- **Gallery**: `gallery.c` is the ngui widget gallery: typography and
  truncation, the drawing primitives and clip stack, the theme palette, all
  nine icons, live buttons/icon buttons, text fields with event feedback, a
  nested draggable scroll pane and the `ngui_cut_*` layout helpers.  The real
  ELF ships in the pack as `bin/gallery.app`; `ucmd_gallery.c` is the tiny
  `bin/gallery` launcher, which prefers `/share/GALLERY` (development upload
  via `make gallery-dev`), then the pack copy, then the `GALLERY` disk
  fallback on `root.img`/FAT mounts.
- **Solitaire**: `ucmd_solitaire.c` — a Windows 95 style Klondike game on the
  canvas service: green felt, a Game/Help menu (ngui buttons and menu items:
  new game, draw one/three, blue or red deck), four foundations, seven tableau
  piles, stock/waste, drag-and-drop (or click a card and then its destination),
  double-click to send a card home, auto-flip, standard scoring, a score/time
  status bar and the bouncing-card win animation.  The cards use pip layouts
  with mirrored corner indices, notched corners and pixel-art J/Q/K portraits;
  the back is the castle.  The bar's "Cards" button launches
  `/ramfs/bin/solitaire`.
- **Browser**: the real browser ships in the pack as `bin/browser.app`
  (it links ngui, the full image library, inflate, TLS and uhttp, but the
  rootfs is no longer constrained by the kernel's low-memory window).
  `ucmd_browser.c` is the tiny `bin/browser` launcher; it prefers
  `/share/BROWSER` (`make browser-dev` uploads it via the board's `websrv`;
  stop `websrv` before browsing HTTP, because it owns the NIC), then the pack
  copy, then `BROWSER` on `root.img`/IDE/SATA fat mounts.  The launcher
  forwards its argument and cwd.
  `uhttp.c/h` is the shared ring-3 HTTP client extracted from curl: URL
  parsing/resolution, redirects, chunked decoding, timeouts and TLS; curl and
  the browser both use it.  `browser_html.c/h` is a tolerant HTML tree + the
  CSS subset: external `<link rel=stylesheet>` sheets (in document order,
  inline `<style>` last), tag/class/id and descendant/child selectors,
  colours/`rgba()`, backgrounds, font size (`px`/`%`/`em`/`rem`), numeric
  weights, `!important`, line-height, alignment, margins, padding, borders,
  `list-style: none`, `text-transform: uppercase`, `display`, percentage
  `width`/`height`/`max-width`, and basic `float: left/right` columns.
  `@media (min-width/max-width)` blocks are evaluated against a fixed 1000 px
  desktop viewport; `@font-face`/`@import`/`@keyframes` are skipped.  Inline
  `<svg>` subtrees and closed `<details>` are skipped (not renderable), and
  `<video>` gets a placeholder box.  The default page canvas follows
  `ngui_theme_dark` (dark page, light text, `theme.panel` for `<pre>`);
  a site's own `background-color`/`color` CSS still overrides it.  `browser.c`
  owns a 960x640 window, the layout/render, URL bar, history, status bar and
  inline `<img>` decoding.  Images are fetched one per main-loop iteration
  (so text and scrolling stay responsive) and only when their measured
  position is within the viewport plus 160 px; deferred images are loaded as
  the page scrolls.  Network work runs in a headless `curl __fetch` helper
  process (`ucmd_curl.c`): it writes the raw body and a small metadata file
  under `/share`, while the browser keeps drawing and scrolling its own
  window.  The helper asks for identity encoding because inflating multi-MB
  pages in the browser process stalled the UI; if a server still sends gzip,
  the browser's inflate has a progress hook that pumps the UI.  Page, CSS and
  image buffers come from the ring-3 libc heap; the DOM uses fixed static
  arenas (6144 nodes, 512 KB attributes, 4608 rules), and page bodies are
  capped at 2 MB with CSS sheets up to 832 KB (six sheets).  Each image gets
  an `img_estimate()`-sized decode arena, freed as soon as the scaled bitmap
  exists; up to 16 bitmaps (512 KB files / 8 MP decodes) are kept until the
  page heap is reset.  Navigation calls `heap_reset()` and unmaps the whole
  arena, so a large image no longer reserves a fixed 24 MB.  There is no
  JavaScript, forms or standalone image viewer mode; flex/grid, basic tables,
  inline SVG and CSS variables are supported (see known issue 22 for the
  remaining CSS gaps).  `SYS_SPAWN` loads FAT ELFs through `kmalloc` now (the
  old 16 KB `fat_load_buf` is gone) and wraps the load in `sys_fs_enter/exit`,
  since the floppy needs IRQ 6.
- **Image library + viewer**: `img.h`, `img.c`, `img_mem.c`, `img_blit.c`,
  `img_png.c`, `img_bmp.c`, `img_gif.c`, `img_jpeg.c`, `img_tga.c`,
  `img_pnm.c`, `img_file.c/h` — freestanding RGB565 decoders with an optional
  8-bit alpha plane.  The codecs are independent of the OS and ngui: they take
  an input buffer plus a caller-provided `struct img_mem` arena, so a browser
  can link the same objects and decode HTTP bodies.  `inflate.c/h` (DEFLATE,
  zlib, gzip, CRC32/Adler32) is shared with the boot pack.  `img_sniff()` is a
  cheap header probe; `img_decode()` returns `struct img_image`; `img_draw()`
  is a clipped, scaled (box-filter down, bilinear up) RGB565 blit with alpha
  over a checkerboard or over the destination (for a browser).  Progressive
  JPEG (successive approximation), EXIF orientation, ICO/CUR, PAM and
  animated GIF frames are supported as well; arithmetic JPEG/CMYK are not.
  The viewer, `ucmd_view.c`, owns a canvas window and reads its file through
  `SYS_READ_FILE`; it uses fit/1:1/zoom/pan, shows format/dimensions/size,
  animates GIFs, and gives a clear error for unsupported variants.  Host
  tests: `make test-images` (`img_test.c`, corpus in `testdata/images/`,
  generator `tools/gen_img_tests.py`).
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

  The server side is the WebDAV command: `tcp.c` has a small listener (one
  active connection + 3 backlog/closing slots, SYN-ACK/RST, and a fast FIN that keeps
  the tuple briefly so a late FIN/ACK is absorbed).  The SYN-ACK advertises a
  zero window; a queued connection may hold 1 KB of early request (usually
  the HTTP headers), `tcp_srv_accept()` opens the window, and the update is
  repeated when the final ACK arrives, so parallel gvfs clients queue
  without RTO stalls.  New SYNs/data are also handed to `tcp_srv_input()`
  from the active connection's `tcp_recv`/`tcp_srv_send` poll loops, not just
  between requests.  `SYS_TCP_LISTEN/ACCEPT/
  SRV_RECV/SRV_SEND/SRV_CLOSE` are called by `/ramfs/bin/websrv`, which serves
  HTTP/1.1 + WebDAV from the VFS (`/` lists mounts, `/ramfs` is read-only,
  FAT mounts accept only names that round-trip to 8.3, `/share` is the
  writable target).  While listening, `SYS_TCP_ACCEPT` also answers ARP/ICMP
  and captures a KXN2/KXNP header through `netkexec_capture_frame()`, so
  `make net-dev`/`make net-cmds` interrupts the server cleanly instead of
  waiting for header retries.  The body is buffered whole (4 MB, or 1 MB when
  the heap alloc fails) from the per-process arena; the TCP receive path sends
  a window update whenever the application drains the queue (otherwise a full
  receive queue deadlocks until the peer's zero-window probe).  No auth: it is
  a trusted-LAN service, and `/share` is writable by anyone who can reach it.
  Responses go out in up to four-segment bursts with a cumulative ACK
  (`TCP_RETRY_MS` 500 ms) rather than one segment per ACK, and PROPFIND XML
  is batched into 8 KB chunks instead of one chunk per entry.  Keep-alive is
  deliberately not used: with one active connection an idle persistent
  connection would block the parallel connections Nautilus opens.

## Shell commands

`cd [path]`, `ls [path]`, `cat <path>`, `save <path> <text>`, `rm <path>`,
`mkdir <path>`, `rmdir <path>`, `hello`, `reboot`, `poweroff`,
`display`, `cpuinfo`, `mouseinfo`, `gfxbench`, `lsusb`, `lspci [id]`, `peek <addr> [len]`, `poke <addr> <val>`, `storage`, `meminfo`, `top`, `sched`, `format <device> [size]` (binary units, e.g. `format ide0 100mb` or `1gb`; no size formats the whole disk),
`ps`, `renice <pid> <weight>`, `kill <pid>`, `idle [auto|hlt|c1e|c3|c6]`, `power`, `powerinfo`, `cpufreq [auto|min|max|off|<ratio>]`, `spin [ms]`, `help`, `clear`, `serial test`, `kexec`, `dbg`, `view <file>` (image viewer; PNG, baseline/progressive JPEG, animated GIF, BMP, TGA, PNM, ICO, PAM), and `browser [url|file]` (disk app launcher).

Network: `netinfo`, `netrecv`, `netsend`, `netip [a.b.c.d]`, `dhcp` (re-run the
DHCP client), `netdev [name]` (kernel command: show the active NIC driver or
switch it, e.g. `netdev asix`), `pong` (responds to ARP/ICMP for our IP for 30 s),
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
boot, if a NIC is present, a DHCP client runs with a 4 s budget (requesting the
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

`cpuinfo`, `ls`, `cat`, `save`, `rm`, `mkdir`, `rmdir`, `meminfo`, `top`, `spin`,
`clock`, `colors`, `files`, `gallery`, `view`, `sensors`, `powerinfo`, `mouseinfo`, `storage`, `netinfo`, `netip`, `netdns`, `host`, `dhcp`, `ping`,
`pong`, `netsend`, `netrecv`, `netlog`, `netconsole`, `netregs`, `netloop`,
`netreset`, `websrv`, `curl`, `solitaire` and `hello` are files in `/ramfs/bin`
(packed by the Makefile), found through the shell's PATH; the rest are kernel
commands spawned as processes, or builtins (`cd`, `path`, `help`, `clear`).

## Serial hot-reload (kexec) workflow

Develop without reflashing the floppy. The serial link (COM1 ↔ `/dev/ttyUSB0`)
runs at **115200 baud**. The WM loop watches the serial ring buffer for the
`"KXE2"` magic and auto-spawns the reload, so `make dev` alone boots a fresh
kernel + rootfs. The manual `kexec` command is kept as a fallback that waits
for the same image.

Wire protocol (host → target, sent immediately; no handshake):
`[magic "KXE2"][entry u32 LE][kernel_size u32 LE][pack_size u32 LE]`
`[kernel][rootfs gzip][checksum u8]`.
`pack_size == 0` tells the receiver to keep the running rootfs (kernel-only
reload). A full reload stages both in one page-allocated buffer, copies only
`kernel_size` bytes over `0x7E00`, and publishes the pack range in boot info;
the new kernel copies/inflates it and frees the staging tail. The rootfs-only
runtime update (`make cmds-dev`) keeps the old framing
`[magic "KXNP"][entry u32][size u32][raw pack][checksum u8]` and replaces
`/ramfs` in place (see below). On SMP the APs are INIT-parked just before the
jump, and the serial receive drops the BKL while it waits for bytes so the WM
and the other CPU's syscalls stay responsive during a `make dev`.

**Every iteration:**
```sh
make dev            # kernel.bin + cmds_pack_gz.bin (atomic; ~46 s at 115200)
make kernel-dev     # kernel only; the new kernel keeps the running rootfs
make cmds-dev       # runtime raw-pack update, no reboot
```
The reload is automatic. On boot the kernel prints `NebOS v<version> booted`
to COM1, so a serial monitor can confirm the new build headlessly:

```sh
make monitor       # tail the serial line (picocom; Ctrl-A Ctrl-X to exit)
```
Run it in a second terminal, then `make dev` in the first — each reload prints
the `... booted` line.

Notes: the kexec path rebuilds a flat GDT at `0x900` before jumping (the old GDT
lives inside the overwritten kernel region), so it's safe to edit any file,
including `vesa_stage.S`/`boot.S` layout — but the video *mode* itself is only
re-applied on a full floppy/USB boot (C-light reuses the current LFB).

## Command pack rootfs (cmds.pack / cmds.img / cmds-dev)

The commands live in `cmds.pack` (`bin/<name>` entries, built by `mkpack`),
compressed with `libdeflate-gzip -12` when it is installed (falling back to
`gzip -9`) to `cmds_pack_gz.bin` and wrapped by `mkrootfs` with a
one-sector `NBROOT1` header (magic, version, exact `pack_bytes`, checksum,
gzip flag).  The build rejects a legacy pack above the `PACK_LOW_MAX` cap;
`libdeflate` buys several KB over `gzip -9` and is currently needed to fit.
The boot path loads the gz pack into RAM — the legacy stage stages it in
the low gap at `0x60000` and relocates it to `0x900000` before the kernel
starts, the UEFI loader via an embedded copy at the top of
RAM below 128 MB, kexec via its staging buffer — and `ramfs_init()` inflates
it into page-allocated memory and mounts it read-only at `/ramfs`.  The
kernel image no longer contains it, so the pack no longer moves `_end`; the
legacy staging gap has its own size cap (known issue 9).

`rootfs.bin` is the floppy append (header + pack); `cmds.img` is the 8 MB
rootfs partition image written to USB partition 2.  `cmds-dev`/`net-cmds`
send the *raw* `cmds.pack`, which `ramfs_replace()` validates and installs
directly in the running kernel.  A running board can replace the pack without
a kernel reload:

```sh
make cmds-dev       # serial:  ./kexec_send  --payload $(SERIAL_DEV) $(SERIAL_BAUD) cmds.pack
make net-cmds       # network: ./netkexec_send --payload $(BOARD_IP) $(NETKEXEC_PORT) cmds.pack
```

The pack is received into page-allocated memory (magic "KXNP" over serial,
the same stop-and-wait protocol with a "KXNP" header over UDP), validated
*before* the swap, so a truncated or corrupt transfer leaves the running
commands untouched; a good one is installed by `ramfs_replace()` and the
previous heap copy is freed.  Existing processes are unaffected (they already
loaded their ELF), and the next spawn picks up the new files.  The WM idle
loop auto-detects the kernel/rootfs (KXE2/KXN2) and the runtime pack (KXNP)
transfers, so a plain `make cmds-dev`/`make net-cmds` is enough.  Runtime
updates are RAM-only; a cold boot uses the flashed rootfs (USB partition 2 or
the floppy append), so reflash after changing the bundle.

## Network hot-reload (net-dev) workflow

For boards without a serial console (the ASUS AT3IONT-I), the same reload runs
over the network instead:

```sh
make net-dev                    # kernel + rootfs to BOARD_IP (default 192.168.178.240)
make net-dev BOARD_IP=192.168.178.42
make kernel-net-dev             # kernel only; keeps the running rootfs
```

The board must be idle at a shell prompt: the WM loop polls the NIC, answers
ARP/ICMP for its IP, and auto-detects the `"KXN2"` header datagram (UDP port
19288).  The header is `magic, entry, kernel_size, pack_size, checksum`; the
host tool (`netkexec_send.c`) sends the kernel then the gz rootfs as one
stop-and-wait stream, the board acks every datagram with the next expected
offset, verifies the 32-bit checksum, writes boot info for the staging tail
and jumps.  `pack_size == 0` keeps the running rootfs.  ~480 KB takes a
couple of seconds on a wired LAN.  While a net command owns the receiver
(`netrecv`/`ping`/`netloop`) or a kexec is in progress, the auto-detect pauses.

The old `KXNU`/`KXEC` magics are deliberately not understood by this kernel
(and it uses `KXN2`/`KXE2`), so a board still running a pre-rootfs build must
be flashed once; the new `make net-dev` is then atomic.

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
pauses while a net command owns the receiver.  The producer path is
`netlog_char` (via `screen_putchar`/`serial_putc`) and is non-blocking;
`SYS_NETLOG` is the direct-send command path — never call it from the WM or
from any BKL holder, because a blocking NIC TX can wedge the whole board.

## USB HID (mouse + keyboard) / host controllers

`usb.c` is controller-agnostic; the drivers implement the `usb_hcd_ops`
vtable (`usb_hcd.h`): control transfers, root-port queries/reset, bulk
transfers and up to six interrupt-IN slots. `usb_hcd_init()` brings up every
controller type and builds one global controller table: EHCI first, then
UHCI, OHCI and xHCI.  A machine with EHCI and UHCI/OHCI companions therefore
enumerates high-speed devices on EHCI and low/full-speed devices on the
companions; a port reset hands a non-high-speed device back to its companion
before the companion is scanned.  A single device address is per-controller,
so addresses do not collide.

- **EHCI**: MMIO BAR0, page-allocated frame list/QH/qTD/bounce buffers, QH/
  qTD descriptors in the identity-mapped kernel space.  Control, bulk and
  interrupt endpoints all live in the asynchronous schedule (a circular QH
  list with the control QH after the reclamation head); the periodic list is
  not used.  Polling only (no IRQs).  Ports are claimed from the companions
  with CONFIGFLAG; `port_reset` holds PORT_RESET for 50 ms, then clears it
  (QEMU and hardware complete the reset on that write), waits for PED and
  sets PORT_OWNER for a non-high-speed device.  `ehci.c` supports up to four
  PCI instances.  The async schedule is enabled only while a control/bulk
  transfer or an armed interrupt endpoint needs it (`ehci_sched_get/put`);
  with nothing attached the controller stops fetching QHs and qTDs, which
  keeps package C-states reachable (the port watcher only reads PORTSC).
  Disabling clears ASE and acknowledges `STS_IAA`; leaving the IAA doorbell
  status set stalls the next enable.  Bulk `in`/`out` use one qTD per call
  (up to five 4 KB buffer pages), which is enough for the ASIX AX88772 RX/TX
  path.
  There is no transaction translator: low/full-speed devices behind a
  high-speed hub stay invisible to EHCI (use the companion controller for
  them).
- **Hubs**: `usb.c` sets a class-9 hub's configuration with
  `SET_CONFIGURATION` before issuing hub-class requests, powers every
  downstream port (honouring the hub descriptor's `bPwrOn2PwrGood`) and waits
  for `PORT_ENABLE` after the port reset.  This is needed for the X201's PCH
  internal `8087:0020` hubs, which sit between the EHCI root ports and the
  external USB ports.  Downstream devices are enumerated recursively; a
  full-speed device behind a USB 2.0 hub would need split transactions (not
  implemented).
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
  terminal/WM see exactly what a PS/2 keyboard would produce.  Since a boot
  keyboard only reports changes, `io.c` adds software typematic repeat
  (500 ms delay, 33 ms rate) for the last make code; real PS/2 hardware
  repeat keeps resetting the same timer.
  `usbhid_poll()` (from `mouse_poll_update()`) also runs the hotplug
  watcher: root-port connect changes are enumerated (throttled to 250 ms),
  `CCS` low or a few TD errors detach a device.  A full scan (`lsusb`) bumps
  `usb_generation()`, which makes `usbhid` detach and rescan, so `lsusb`
  can't leave the devices dead.  The BIOS PS/2 legacy emulation is
  deliberately disabled (PCI config `0xC0`) because the kernel now owns
  these devices.
- **Debug**: `ohci:` / `uhci:` / `ehci:` / `usbhid:` lines go to COM1 (and
  through the netlog mirror on serial-less boards); `mouseinfo` shows the
  active mouse backend and whether a USB keyboard is attached; `usb.c`'s
  `USB_DEBUG` and `usbhid.c`'s `USBHID_DEBUG` (both default 0) add
  scan/report tracing.  Headless tests: `make test-usb` runs
  `tools/qmp_cursor_test.py` (injects mouse motion with QMP
  `input-send-event`, verifies the cursor with `screendump`) and
  `tools/qmp_keyboard_test.py` (types with QMP `send-key`, verifies the
  screen output); the cursor test takes `--uhci`, `--uhci72` and `--multi`,
  and both take `--hotplug`, `--xhci` and `--ehci`.
- **Limits**: EHCI drives high-speed root-port devices only (no TT/split
  transactions for low/full-speed devices behind a high-speed hub), hotplug
  watches root ports only (a device behind a hub is only enumerated when a
  scan runs), HID report IDs are not handled (a device that prepends report
  IDs decodes as garbage), absolute pointers/tablets are ignored, no wheel
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
   The `NebOS v<version> booted` line is already written to COM1 at boot, so
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

## Fatal exception screen (`panic`)

The on-screen debug overlay (`kdbg`: live counters, heartbeat, boot info rows)
has been removed.  The one diagnostic screen left is the fatal exception
banner: on a ring-0 fault, `exc_handler()` (`kernel.c`) calls
`panic_report()` (`panic.c/h`), which draws straight into the VESA LFB
(bypassing the shadow buffer and the WM) and mirrors the same data to COM1,
then halts.  Faults from ring 3 do not halt: the process is killed (with a
short message in its terminal and on serial) and the kernel keeps running.

What the banner and the COM1 dump contain:

- vector name/number and the CPU error code (`#PF` also decodes P/W/U/RSVD/ID),
- EIP, CS (ring), EFLAGS, and the faulting ESP (ring 0) or user ESP/SS,
- EAX..EDI and EBP,
- CR0, CR2, CR3, CR4,
- current pid/name and uptime ticks,
- up to 16 bytes at EIP (read only from mapped RAM and clipped at the page
  boundary, so the dump cannot fault again),
- a few stack dwords plus return-address candidates found by scanning the
  faulting stack page for values in the kernel image (`0x7E00..__bss_end`).
  `-O2` omits frame pointers, so there is no EBP-chain backtrace; the
  candidates are hints, not a true trace.

COM1 additionally gets 32 stack dwords and every candidate, each with its
offset from the faulting ESP.  Addresses are raw kernel addresses: resolve
them with `nm kernel.elf` / `addr2line -e kernel.elf <addr>`.

Wiring: `timer.S` builds a `struct exc_state` (GPRs via `pusha` and the CPU's
exception frame, including the error code that the old stubs discarded) and
passes it to `exc_handler()`; `panic.h` documents the layout.  The banner
caches the LFB geometry lazily, so no boot hook is needed.  The MTRR dump and
the NIC bring-up trail that used to fill the info rows now go to COM1 only
(`mtrr.c`, `r8168.c`).  The IRQ1 storm guard still masks a storming keyboard
and the WM falls back to polling (`io_keyboard_irq_masked()`).

## Conventions & gotchas

- **Freestanding C** — the kernel has no libc, no `malloc`, no `printf`; use
  `io_putstr` / `io_putchar` / `io_puthex` (`io.h`).  Ring-3 commands may
  link the in-tree `libc.a` (`malloc`, strings, stdlib); a command either uses
  libc malloc or the raw `SYS_MEM_*` arena, never both.
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
6. Killing/reaping a ring-3 process frees its kernel stack and page directory
   but not the user pages mapped into it (a few pages leak per command run;
   needs a page-table walk/unmap on reap).
7. `display` (needs a GPU snapshot) and `lsusb` (a 45-byte wrapper around
   the USB tree printer) are the last non-privileged commands still living in
   the kernel, plus the privileged ones (`peek`, `poke`, `dbg`, `serial`,
   `kexec`, `reboot`, `poweroff`, `format`, `ps`/`kill`).
8. The rootfs pack is inflated into one contiguous page-allocated run at
   boot, capped by `RAMFS_MAX_DECOMPRESSED` (8 MB); a larger pack fails to
   load (the USB rootfs partition is 8 MB, so this only bites if the cap is
   raised without a bigger partition).  The legacy stage still stages the
   compressed pack through the `0x60000..0x9FBFF` gap before copying it to
   `0x900000`, so `PACK_LOW_MAX` (0x3FBFF, ~255 KB compressed) still caps the
   legacy path; a larger pack needs the UEFI/kexec path or a chunked
   high-memory loader.  The browser and its ring-3 dependencies are compiled
   with function sections and linked with `--gc-sections`; the current pack
   is ~242 KiB (247493 bytes), leaving ~13 KB under the cap.  Command growth
   still needs care because `libdeflate-gzip -12` is already required (plain
   `gzip -9` is over the cap).
9. SMP is **ACPI/MADT only**: no Intel MP-table fallback, so a Pentium II SMP
    board that exposes only MP tables stays UP (deliberate; there is no test
    hardware).
10. There is no IOAPIC driver.  The BSP keeps the 8259 PIC in LAPIC ExtINT mode;
    APs mask LINT0/LINT1 and only use their LAPIC timer/IPIs.  Device IRQs never
    reach an AP, and `pit_irq_stub` should be treated as BSP-only.
11. The BKL serializes all kernel execution: a long FAT/disk operation, a
    non-network driver wait or a big shadow-buffer operation can stall syscalls
    on the other CPUs for its duration.  `bkl_pause()`/`bkl_resume()` is used
    for serial/network waits, not yet for disk I/O.
12. Kernel threads (kworker, kexec/payload, kernel commands) are pinned to
    the BSP.  Only ring-3 processes use the APs, so a single CPU-bound kernel
    driver still limits throughput.
13. Page-table updates for another CPU's running process (e.g. an MMIO mapping)
    rely on not-present TLB entries not being cached; there is no cross-CPU
    TLB shootdown.  No process shares an address space with another, so normal
    process teardown is unaffected.
14. Image library limits: arithmetic JPEG and CMYK are rejected; EXIF
    orientation is applied but ICC/gamma is ignored; animated GIF frames are
    composited (the viewer plays them), but APNG/WebP remain unsupported.  The
    whole file is read into memory (4 MB cap in the viewer) and very large
    photos still need DCT-scaled/streamed decode before they fit the 32 MB
    arena.
15. `img_draw()`'s box-filter downscale runs on the CPU.  It is fine for the
    viewer's static frames, but a browser showing many large inline images
    should cache a scaled bitmap (or use decode-at-display-scale) instead of
    rescaling on every repaint.
16. `files` recognises images by file extension only (`.png .jpg .jpeg .gif
    .bmp .tga .ppm .pgm .pbm .ico .cur .pam`); a magic-based fallback would
    also open extensionless files in the viewer.  Automatic decode-at-scale
    for oversized images is still missing.

17. WebDAV limits: one request at a time (one active connection plus 3
    backlog/closing slots; the queue holds each connection's first 1 KB with a zero window),
    files are buffered whole (4 MB cap; a larger PUT gets 413), directory
    `MOVE` is 501, `PROPPATCH` is answered 200 without persisting, and there
    is no authentication.  Sends use a four-segment window, and keep-alive is
    off, so parallel gvfs clients serialize but do not wedge.
18. FAT mounts are writable over WebDAV only for names that
    `fat_name_to_8_3()` can represent exactly; anything longer is refused
    with 409 instead of silently truncating (two long names could otherwise
    collide and overwrite each other).  Use `/share` for long names.
19. The WebDAV server and the packet-level net commands are mutually
    exclusive: both need the single NIC receiver (`netdev_acquire`).  Starting
    `websrv` while `ping`/`curl`/`netrecv` owns the NIC fails; kill the other
    command first.
20. `/share` is RAM only: a reboot/kexec starts empty, and the 16 MB cap is
    charged against the 128 MB identity-mapped window.

21. The TCP backlog's early-request buffers cost 8 KB of kernel BSS
    (8 slots x 1 KB), and the FAT listing cache another 5 KB.  The command
    pack is out of the kernel now, so `_end` keeps ~190 KB of headroom below
    the `0x90000` wall; the remaining limit is kernel code/buffers only.

22. Browser limits: no JavaScript, forms, pseudo-classes/elements,
    border-radius clipping of descendant content, gradients/box-shadow/
    transitions (a gradient background falls back to its last solid colour),
    or CSS `color-mix`.  CSS custom properties (`var()`), flex rows with
    wrap/grow/gap, two-track and `repeat(auto-fit, minmax(...))` grids,
    inline-block runs, basic proportional table columns and inline SVG
    (`rect`, `line`, `path` with M/L/H/V, and `text`) are supported.
    `position:fixed` boxes are deferred and painted above the flow;
    `position:sticky` is treated as static.  `prefers-color-scheme` media
    queries are ignored so the page's base palette is used.  `width`/
    `max-width` are not applied to block flow (they collapsed nested
    percentage columns); flex/grid items still use them.  Float support is
    the basic Bootstrap-grid case: consecutive floated blocks are laid side
    by side with percentage/px widths, `float:right` is treated like left,
    there is no `clear`, percentage margins are approximated as pixels, and
    a float without a width uses a text-width estimate.  `@media` width
    blocks are evaluated against a fixed 1000 px desktop viewport (so
    narrow/adaptive pages get the desktop rules, not their mobile layout),
    and pseudo-classes are never matched.  `font-size` maps to 1x/2x only,
    `em`/`rem` both use a 16 px root, and `line-height` is approximate.
    Pages that need flex alignment/justification, multi-row grid templates
    or CSS variables inside `calc()`/`color-mix()` can still lay out
    imperfectly.  Images are decoded into a per-image `img_estimate()`-sized
    arena that is freed after the 1000 px display bitmap is made (16 images,
    512 KB files / 8 MP decodes); a large image-heavy page can still exhaust
    the 32 MB heap because the scaled bitmaps live until navigation, and the
    decode arena itself can transiently be large.  `img_draw` rescales the
    cached bitmap on every repaint.  Lazy
    image loading uses the last measured positions; adding an image can
    reflow the content above it, and images below the viewport stay as
    `[image]` until scrolled into range.  Each network fetch starts a
    one-shot helper process and opens a fresh TCP/TLS connection (the
    `uhttp` keep-alive path exists but the browser does not use it across
    helper jobs), so many tiny resources have extra process/connection
    overhead.  The helper deliberately requests identity encoding, so a page
    that is only small when compressed can exceed the 2 MB page/832 KB CSS
    caps.  Video/audio elements are placeholders.  Bare `&` characters are
    consumed by the entity decoder instead of looping forever.  Because the
    default canvas is dark, a page that sets dark text but relies on the
    browser's default white background can be hard to read; pages with their
    own background and colour rules are unaffected.  TLS is phase A (no
    certificate verification); `https` is shown as unverified.  The disk app
    is loaded from FAT at spawn time, so a cold floppy start takes a few
    seconds; the 8 KB user stack means the layout state is deliberately
    static.

23. UEFI boot is x86-64 firmware only (no `BOOTIA32.EFI` yet) and the loader
    is unsigned, so Secure Boot must be off.  The loader needs a GOP mode with
    BGRX or RGBX 32bpp, width ≤ 1280, `pitch * height` ≤ 8 MB and the
    framebuffer below 4 GB; otherwise the kernel starts headless (serial only).
    xHCI drives USB2 root ports only (USB3 SuperSpeed devices are not
    handled), and EHCI only high-speed root ports (no transaction translator
    for low/full-speed devices behind a high-speed hub), so a USB3-only
    keyboard/mouse can still leave the UI without input (PS/2 may still work).
    There is no IOAPIC setup: OVMF delivers the ISA IRQs through the 8259
    path, but a platform that routes them through the IOAPIC may need a
    mask-all step after ACPI init.  UEFI runtime services are not used, so
    `reboot`/`poweroff` still use the legacy ports.  The image is 17 MB: the
    9 MB kernel + rootfs disk and the 8 MB ESP.

24. ThinkPad EC sensor and battery access (`tpec.c`) runs in legacy/SMM mode
    because there is no ACPI interpreter: an SMI that touches the EC between
    our port accesses can produce an occasional bad sample (the battery
    reader validates its values and the next 1 Hz poll corrects the bar), and
    the 0x81 page-selector write can race the firmware's own battery poll.
    EC hotkey events are drained/discarded instead of handled.  The PCH thermal sensor
    (`8086:3B32`, the `intel_ips` MMIO block) is not read because it needs
    the ME/BIOS to have enabled thermal reporting; the EC and DTS readings
    cover the package temperature.

25. Ring-3 libc heap limits: one process owns the single 32 MB `SYS_MEM_*`
    arena, there is no `free()`-to-page unmap (only `heap_reset()`), no
    threads/locking and no double-free recovery beyond the block magic; a
    pointer into the middle of a block or a stale pointer is silently
    ignored by `free()`.  The allocator is first-fit with immediate
    coalescing, so a pathological small/large lifetime mix can still
    fragment; the browser and viewer use it for a handful of large,
    page-scoped buffers.  A program must not mix `malloc` with raw
    `u_mem_alloc`/`u_mem_grow`/`u_mem_free`, and near-32 MB requests fail
    because the block headers are charged to the same cap.  The `libc.a`
    members are only pulled into commands that reference them; the legacy
    pack still needs every added byte checked against `CMDS_GZ_MAX`.
