#!/usr/bin/env python3
"""USB keyboard and mouse, end to end, on aarch64 -- docs/RPI4.md P5.

    tools/arm64_usb_input_test.py <kernel.img> <kernel.elf> <rootfs.img> <log> [--hub]

--hub puts the keyboard and the mouse behind a USB hub, which is how every
Raspberry Pi 4 has them: its USB 2.0 ports are a hub inside the VL805. That
exercises the route string, the hub's port power/reset/status, and full-speed
enumeration (EP0's packet size learned and set with Evaluate Context).
QEMU's usb-hub is full speed, so a high-speed hub's Transaction Translator --
the Pi's -- is the one part only the board can test.

Used by `make ARCH=aarch64 test-arm64-usb`.

A Raspberry Pi 4's keyboard and mouse are USB, behind an xHCI (the VL805)
behind its PCIe. QEMU's raspi4b has no PCIe, so the Pi half of that is for the
real board -- but the USB half is the same code on any aarch64 machine, and
QEMU `virt` can carry it: a qemu-xhci with a usb-kbd and a usb-mouse, and NO
virtio input, so the only way a key or a motion can reach the kernel is the
path this test is about.

Three witnesses, each one a thing the log alone cannot prove:

  * the SERIAL log: both devices configured as boot-protocol HID (with
    SET_PROTOCOL sent -- a real mouse needs it), and the controller polled;
  * a KEY pressed through QEMU's input layer (QMP input-send-event) must raise
    the xHCI driver's keyboard-report counter, read out of guest memory;
  * a relative MOTION of (+100, +50) must raise the mouse-report counter AND
    move the kernel's cursor by exactly that -- the report was parsed, not just
    received, and its axes and signs are the right way round.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000
TIMEOUT_S = 180
NM = os.environ.get("NM", "aarch64-elf-nm")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def symbol(elf, name, in_file=None):
    """A symbol's address. `in_file` picks one of several same-named statics."""
    out = subprocess.run([NM, "-l", elf], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name and \
                (in_file is None or (len(parts) > 3 and in_file in parts[3])):
            return int(parts[0], 16)
    raise SystemExit("arm64_usb_input_test: no symbol %s%s in %s"
                     % (name, " (%s)" % in_file if in_file else "", elf))


class Qmp:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.f = socket.create_connection(("127.0.0.1", port)).makefile("rw")
                break
            except OSError:
                time.sleep(0.1)
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.f.write(json.dumps(m) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" in r:
                continue
            if "error" in r:
                raise RuntimeError(r["error"])
            return r

    def read(self, va, fmt):
        """Read a kernel-image variable by its PHYSICAL address (xp), which
        needs no guest page tables and cannot fault."""
        n = {"g": 8, "w": 4}[fmt]
        out = self.cmd("human-monitor-command",
                       **{"command-line": "xp /1%sx 0x%x" % (fmt, va - KERNEL_VIRT_BASE)})["return"]
        v = int(out.split(":")[1].split()[0], 16)
        return v - (1 << (8 * n)) if fmt == "w" and v >= 1 << 31 else v

    def key(self, qcode):
        for down in (True, False):
            self.cmd("input-send-event", events=[
                {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}])
            time.sleep(0.05)

    def move(self, dx, dy):
        self.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}])


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    kernel, elf, rootfs, log = sys.argv[1:5]
    hub = sys.argv[5:] == ["--hub"]
    usb = (["-device", "usb-hub,bus=xhci.0,port=1",
            "-device", "usb-kbd,bus=xhci.0,port=1.1",
            "-device", "usb-mouse,bus=xhci.0,port=1.2"] if hub else
           ["-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0"])
    kbd_va = symbol(elf, "g_hid_kbd_reports")
    mouse_va = symbol(elf, "g_hid_mouse_reports")
    x_va = symbol(elf, "g_x", "mouse.c")
    y_va = symbol(elf, "g_y", "mouse.c")

    hvf = sys.platform == "darwin" and os.uname().machine == "arm64"
    accel = ["-M", "virt,gic-version=3,accel=hvf", "-cpu", "host"] if hvf else \
            ["-M", "virt,gic-version=3", "-cpu", "cortex-a72"]
    port = free_port()
    if os.path.exists(log):
        os.remove(log)
    qemu = subprocess.Popen(
        ["qemu-system-aarch64", *accel, "-smp", "4", "-m", "512M",
         "-display", "none", "-monitor", "none", "-serial", "file:" + log,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port,
         "-drive", "file=%s,format=raw,if=none,id=d0,snapshot=on" % rootfs,
         "-device", "virtio-blk-pci,drive=d0", "-device", "virtio-gpu-pci",
         "-device", "qemu-xhci,id=xhci", *usb,
         "-kernel", kernel],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fail = 0

    def check(ok, what):
        nonlocal fail
        fail |= not ok
        print("  [%s]  %s" % ("ok" if ok else "FAIL", what))

    try:
        q = Qmp(port)
        deadline = time.time() + TIMEOUT_S
        text = ""
        while time.time() < deadline:
            text = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "home: desktop ready" in text or "all self-tests done" in text:
                break
            time.sleep(1)
        time.sleep(2)                 # the boot loop is polling USB by now
        text = open(log, errors="replace").read()

        # The xHCI's DMA state remapped non-cacheable (a Pi 4's PCIe does not
        # snoop the caches), and the kernel's own read-back of the page
        # tables found nothing that stayed cacheable.
        check("mapped for device access" in text and
              "did NOT become non-cacheable" not in text,
              "xHCI DMA state mapped non-cacheable, verified from the page tables")
        if hub:
            for m in ("USB Hub detected", "hub slot1: 8 port(s)",
                      "route=1", "route=2", "full-speed EP0 is 8 bytes a packet"):
                check(m in text, m)
        for m in ("HID Boot Keyboard", "HID Boot Mouse",
                  "SET_PROTOCOL(boot) on interface 0 OK",
                  "HID keyboard ready", "HID mouse ready",
                  "polled (no interrupt wiring on this architecture)"):
            check(m in text, m)

        k0, m0 = q.read(kbd_va, "g"), q.read(mouse_va, "g")
        x0, y0 = q.read(x_va, "w"), q.read(y_va, "w")

        for c in "emb":
            q.key(c)
        time.sleep(1)
        k1 = q.read(kbd_va, "g")
        check(k1 > k0, "keys pressed in QEMU reached the xHCI driver "
              "(%d -> %d keyboard reports)" % (k0, k1))

        q.move(100, 50)
        time.sleep(1)
        m1 = q.read(mouse_va, "g")
        x1, y1 = q.read(x_va, "w"), q.read(y_va, "w")
        check(m1 > m0, "mouse motion reached the xHCI driver "
              "(%d -> %d mouse reports)" % (m0, m1))
        check((x1 - x0, y1 - y0) == (100, 50),
              "the cursor moved by exactly (+100, +50): (%d,%d) -> (%d,%d)"
              % (x0, y0, x1, y1))
    finally:
        qemu.terminate()
        qemu.wait()

    where = "behind a hub" if hub else "on root ports"
    if fail:
        print("test-arm64-usb (%s): FAILED -- serial log in %s" % (where, log))
        sys.exit(1)
    print("test-arm64-usb: USB keyboard and mouse work on aarch64, %s (log: %s)"
          % (where, log))


if __name__ == "__main__":
    main()
