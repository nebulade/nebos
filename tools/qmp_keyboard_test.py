#!/usr/bin/env python3
"""Headless USB keyboard regression test (QEMU + QMP + screendump).

Boots the OS, types a shell command on a USB keyboard through QMP and
verifies the screen content changed (the command output).

Modes:
  (default)   OHCI + usb-mouse + usb-kbd; types "help"
  --hotplug   boots without the keyboard, plugs it in via QMP, then types
  --keys a,b,ret   type a custom key list (QEMU qcodes)

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


def main():
    hotplug = "--hotplug" in sys.argv
    xhci = "--xhci" in sys.argv
    keys = ["h", "e", "l", "p", "ret"]
    if "--keys" in sys.argv:
        keys = sys.argv[sys.argv.index("--keys") + 1].split(",")

    if not (os.path.exists(os.path.join(ROOT, "floppy.img")) and
            os.path.exists(os.path.join(ROOT, "root.img"))):
        print("floppy.img/root.img missing - run `make all` first")
        return 1

    workdir = tempfile.mkdtemp(prefix="nebos-kbd-")
    sock = os.path.join(workdir, "qmp.sock")
    ppm = os.path.join(workdir, "screen.ppm")

    kbddev = [] if hotplug else ["-device", "usb-kbd"]
    qemu = ["qemu-system-i386", "-M", "pc"]
    if xhci:
        qemu += ["-device", "qemu-xhci", "-device", "usb-mouse"]
    else:
        qemu += ["-device", "pci-ohci", "-device", "usb-mouse"]
    qemu += kbddev + [
        "-drive", "file=floppy.img,format=raw,if=floppy",
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
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        qmp_recv(s)                  # greeting
        qmp_cmd(s, "qmp_capabilities")
        time.sleep(4)                # boot

        if hotplug:
            qmp_cmd(s, "device_add", {"driver": "usb-kbd", "id": "kbd"})
            time.sleep(2)

        if os.path.exists(ppm):
            os.unlink(ppm)
        qmp_cmd(s, "screendump", {"filename": ppm})
        time.sleep(0.5)
        w, h, before = read_ppm(ppm)

        for k in keys:
            qmp_cmd(s, "send-key", {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.25)
        time.sleep(1.0)

        if os.path.exists(ppm):
            os.unlink(ppm)
        qmp_cmd(s, "screendump", {"filename": ppm})
        time.sleep(0.5)
        w2, h2, after = read_ppm(ppm)

        changed = sum(1 for a, b in zip(before, after) if a != b)
        ok = changed > 3000          # the command output repaints the screen
        print("keys=%s screen %dx%d changed bytes: %d -> %s"
              % (",".join(keys), w, h, changed, "ok" if ok else "NO CHANGE"))

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
