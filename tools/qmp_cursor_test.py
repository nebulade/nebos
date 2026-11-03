#!/usr/bin/env python3
"""Headless USB HID cursor regression test (QEMU + QMP + screendump).

Boots the OS with a USB mouse, injects motion through QMP and verifies the
on-screen cursor triangle lands where the injected deltas say it should.

Modes:
  (default)   OHCI controller, mouse on it
  --uhci      UHCI (piix3) controller
  --uhci72    UHCI at PCI 00:01.2 (multifunction south-bridge layout)
  --multi     three OHCI controllers, mouse on the third
  --xhci      xHCI controller, mouse on it
  --ehci      EHCI + UHCI companion, mouse on the companion (multi-HCD)
  --hotplug   boot without the mouse, plug it in, move, unplug, replug

Requires: floppy.img/root.img built (make all), python3, QEMU.
Exit code 0 = pass.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 16x16 cursor fill rows (same table as cursor_shape.h); bit 15 = column 0.
CUR = [0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
       0x7F80, 0x7E00, 0x7000, 0x4000, 0x0000, 0x0000, 0x0000, 0x0000]


def qmp_connect(sock):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    qmp_recv(s)                  # greeting
    qmp_cmd(s, "qmp_capabilities")
    return s


def qmp_recv(s):
    buf = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("QMP connection closed")
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line:
                return json.loads(line)


def qmp_cmd(s, execute, arguments=None):
    req = {"execute": execute}
    if arguments is not None:
        req["arguments"] = arguments
    s.sendall((json.dumps(req) + "\n").encode())
    while True:
        msg = qmp_recv(s)
        if "return" in msg or "error" in msg:
            if "error" in msg:
                print("QMP error:", msg)
            return msg


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = [int(x) for x in parts[1].split()]
    return w, h, parts[3]


def pixel(w, pix, x, y):
    o = (y * w + x) * 3
    return pix[o], pix[o + 1], pix[o + 2]


def cursor_matches(w, pix, x0, y0):
    for r in range(16):
        for c in range(16):
            want = (CUR[r] & (0x8000 >> c)) != 0
            got = pixel(w, pix, x0 + c, y0 + r) == (255, 255, 255)
            if want != got:
                return False
    return True


def find_cursor(w, h, pix):
    for y in range(h - 16):
        for x in range(w - 16):
            if cursor_matches(w, pix, x, y):
                return (x, y)
    return None


def screendump(s, ppm):
    if os.path.exists(ppm):
        os.unlink(ppm)
    qmp_cmd(s, "screendump", {"filename": ppm})
    time.sleep(0.5)
    return read_ppm(ppm)


def move(s, dx, dy):
    """Inject relative motion, split so no 8-bit HID field wraps."""
    while dx or dy:
        ex = max(-100, min(100, dx))
        ey = max(-100, min(100, dy))
        qmp_cmd(s, "input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": ex}},
            {"type": "rel", "data": {"axis": "y", "value": ey}}]})
        dx -= ex
        dy -= ey
        time.sleep(0.3)


def main():
    mode = "ohci"
    for m in ("uhci", "uhci72", "multi", "hotplug", "xhci", "ehci"):
        if "--" + m in sys.argv:
            mode = m

    if not (os.path.exists(os.path.join(ROOT, "floppy.img")) and
            os.path.exists(os.path.join(ROOT, "root.img"))):
        print("floppy.img/root.img missing - run `make all` first")
        return 1

    workdir = tempfile.mkdtemp(prefix="nebos-cursor-")
    sock = os.path.join(workdir, "qmp.sock")
    ppm = os.path.join(workdir, "screen.ppm")

    qemu = ["qemu-system-i386", "-M", "pc"]
    if mode == "uhci":
        qemu += ["-device", "piix3-usb-uhci", "-device", "usb-mouse"]
    elif mode == "uhci72":
        qemu += ["-device", "piix3-usb-uhci,addr=1.2", "-device", "usb-mouse"]
    elif mode == "multi":
        qemu += ["-device", "pci-ohci,id=o0", "-device", "pci-ohci,id=o1",
                 "-device", "pci-ohci,id=o2", "-device", "usb-mouse,bus=o2.0"]
    elif mode == "hotplug":
        qemu += ["-device", "pci-ohci"]
    elif mode == "xhci":
        qemu += ["-device", "qemu-xhci", "-device", "usb-mouse"]
    elif mode == "ehci":
        qemu += ["-device", "usb-ehci,id=ehci0",
                 "-device", "piix3-usb-uhci,id=uhci0",
                 "-device", "usb-mouse,bus=uhci0.0"]
    else:
        qemu += ["-device", "pci-ohci", "-device", "usb-mouse"]
    qemu += ["-drive", "file=floppy.img,format=raw,if=floppy",
             "-drive", "file=root.img,format=raw,if=floppy",
             "-display", "none",
             "-qmp", "unix:%s,server,nowait" % sock]

    proc = subprocess.Popen(qemu, cwd=ROOT, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    ok = False
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        s = qmp_connect(sock)
        time.sleep(4)                       # boot

        if mode == "hotplug":
            qmp_cmd(s, "device_add", {"driver": "usb-mouse", "id": "mouse"})
            time.sleep(2)

        w, h, pix = screendump(s, ppm)
        start = find_cursor(w, h, pix)
        move(s, 250, 150)
        time.sleep(0.5)
        w, h, pix = screendump(s, ppm)
        end = find_cursor(w, h, pix)

        if mode == "hotplug":
            qmp_cmd(s, "device_del", {"id": "mouse"})
            time.sleep(1)
            qmp_cmd(s, "device_add", {"driver": "usb-mouse", "id": "mouse"})
            time.sleep(2)
            move(s, -100, -60)
            time.sleep(0.5)
            w, h, pix = screendump(s, ppm)
            end2 = find_cursor(w, h, pix)
            ok = (start == (100, 100) and end == (350, 250) and
                  end2 == (250, 190))
            print("mode=%s fb=%dx%d boot=%s plug=%s replug=%s -> %s"
                  % (mode, w, h, start, end, end2,
                     "ok" if ok else "MISMATCH"))
        else:
            ok = start == (100, 100) and end == (350, 250)
            print("mode=%s fb=%dx%d cursor %s -> %s : %s"
                  % (mode, w, h, start, end, "ok" if ok else "MISMATCH"))

        qmp_cmd(s, "quit")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        shutil.rmtree(workdir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
