#!/usr/bin/env python3
"""usb_hotplug.py -- push a stick in while it is running, and take it out.

    make test-usb-hotplug

Until now every filesystem this OS mounted was found once, during boot, by
code that knew where to look. That is the right shape for the root filesystem
and the wrong shape for a USB port, whose entire purpose is that you can use it
whenever you like. docs/PILLARS.md scoped it exactly: "ports scanned once at
boot; no hot-plug -- a USB stick plugged in after boot does nothing".

THE TEST IS THE DIFFERENCE, and it is deliberately measured on THIS side rather
than in the guest. The guest cannot assert "there is a stick attached", because
on a real machine that is the owner's business and no test should fail for it.
What the guest can do is REPORT -- devices, block devices, mounts, and the first
name it can read out of each mount. The host attached the stick, so the host
knows what the report should say before and after.

Four readings, and the middle two are the ones that matter:

    empty  ->  plug in  ->  it appears and can be READ  ->  unplug  ->  it is gone

A driver that enumerates on plug but never notices the unplug passes the first
three and fails the last, and it is the one that leaves the VFS dispatching
reads at a volume whose medium is in somebody's pocket.

THE IMAGE IS BUILT HERE, with a FAT32 filesystem and a file whose name the
guest has to read back. "The filesystem was recognised" and "the medium is
readable" are different claims and only the second one is worth having.
"""
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from mkfat32 import build_fat32

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
STICK = os.path.join(BUILD, "usbstick.img")
SOCK = os.path.join(BUILD, "usbhp-ser.sock")
QMP = os.path.join(BUILD, "usbhp-qmp.sock")

# Which host controller to plug into. UHCI by default, because it is the one
# the shared core has always covered.
HC = sys.argv[1] if len(sys.argv) > 1 else "piix3-usb-uhci"
BUS = "uhc.0"

# The file the guest must find. Short and 8.3-clean so it survives FAT32's
# short-name rules without depending on long-name support being right.
MARKER = "HOTPLUG.TXT"


def make_stick():
    """A 32 MiB FAT32 volume with one file in the root.

    Built by tools/mkfat32.py, which writes the bytes itself: the build host is
    macOS, where the Linux tools this tree deliberately does not depend on are
    absent (see docs/TODO.md on sfdisk and mkfs.vfat)."""
    build_fat32(STICK, 32, {MARKER: b"plugged in while it was running\n"},
                label="EMBSTICK")
    print("usb-hotplug: built %s (32 MiB FAT32, holding %s)"
          % (os.path.relpath(STICK, ROOT), MARKER))


class Qmp:
    def __init__(self, path):
        for _ in range(60):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(1)
        else:
            raise SystemExit("usb-hotplug: QMP never came up")
        self.f = self.s.makefile("rwb")
        self.f.readline()                       # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, _command, **args):
        import json
        msg = {"execute": _command}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("usb-hotplug: QMP closed")
            r = json.loads(line)
            if "event" in r:
                continue
            if "error" in r:
                raise SystemExit("usb-hotplug: QMP error: %s" % r["error"])
            return r


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("usb-hotplug: another qemu-system is running; refusing to start a second one")
        return 2

    make_stick()
    for p in (SOCK, QMP):
        if os.path.exists(p):
            os.remove(p)
    scratch = os.path.join(BUILD, "usbhp-root.img")
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch], check=True)

    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        # A CONTROLLER WITH NOTHING ON IT. The stick is added later, which is
        # the whole point -- a device present at boot proves only that the
        # boot-time scan still works. Which controller is the argument: UHCI
        # shares kernel/drivers/usb/usb_core.c with OHCI and EHCI, and xHCI
        # has its own enumeration entirely, so "hot-plug works" has to be
        # shown separately for each.
        "-device", "%s,id=uhc" % HC,
        "-drive", "format=raw,file=%s,if=none,id=stickdrv" % STICK,
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "usbhp-qemu.log"), "wb")
    p = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)

    fails = []
    try:
        s = None
        for _ in range(90):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(SOCK)
                break
            except OSError:
                time.sleep(1)
        if s is None:
            raise SystemExit("usb-hotplug: the guest never opened its serial socket")
        s.settimeout(0.4)
        buf = bytearray()

        def drain(seconds):
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass

        def report(tag):
            buf.clear()
            s.sendall(b"test usbdevs\n")
            drain(12)
            text = buf.decode("utf-8", "replace")
            m = re.findall(r"\[usb\] (\d+) device\(s\) enumerated, (\d+) block "
                           r"device\(s\), (\d+) removable volume\(s\) mounted", text)
            if not m:
                fails.append("%s: the guest never answered `test usbdevs`" % tag)
                return None, text
            devs, blocks, mounts = (int(x) for x in m[-1])
            print("usb-hotplug: %-14s %d usb device(s), %d block device(s), "
                  "%d mount(s)" % (tag, devs, blocks, mounts))
            return (devs, blocks, mounts), text

        drain(70)
        before, _ = report("empty port")

        q = Qmp(QMP)
        q.cmd("device_add", driver="usb-storage", drive="stickdrv",
              id="stick", bus=BUS)
        print("usb-hotplug: stick attached over QMP")
        time.sleep(6)
        after, text = report("plugged in")

        if before and after:
            if after[0] <= before[0]:
                fails.append("THE PLUG WAS NOT NOTICED: %d USB device(s) before, "
                             "%d after" % (before[0], after[0]))
            if after[1] <= before[1]:
                fails.append("no block device appeared (%d -> %d)"
                             % (before[1], after[1]))
            if after[2] <= before[2]:
                fails.append("nothing was MOUNTED (%d -> %d) -- the medium was "
                             "found but its filesystem was not"
                             % (before[2], after[2]))

        # AND IT HAS TO BE READABLE. A mount whose directory cannot be listed
        # is a recognised superblock and nothing more.
        if MARKER not in text:
            fails.append("the guest never read %s off the stick -- mounted is "
                         "not the same as readable" % MARKER)
        else:
            print("usb-hotplug: the guest read %s off the new volume" % MARKER)
        m = re.search(r"mount (\S+) <- (\S+) \((\w+)\)", text)
        if m:
            print("usb-hotplug: mounted %s from %s as %s"
                  % (m.group(1), m.group(2), m.group(3)))

        q.cmd("device_del", id="stick")
        print("usb-hotplug: stick removed over QMP")
        time.sleep(6)
        gone, _ = report("unplugged")
        if before and gone:
            if gone[0] != before[0]:
                fails.append("THE UNPLUG WAS NOT NOTICED: %d USB device(s), "
                             "expected back to %d" % (gone[0], before[0]))
            if gone[1] != before[1]:
                fails.append("the block device outlived the medium (%d, expected "
                             "%d)" % (gone[1], before[1]))
            if gone[2] != before[2]:
                fails.append("the mount outlived the medium (%d, expected %d)"
                             % (gone[2], before[2]))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== test-usb-hotplug: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
