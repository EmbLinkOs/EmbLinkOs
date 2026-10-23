#!/usr/bin/env python3
"""netlink_shot.py -- does the OS notice when you unplug the cable?

    python3 tools/netlink_shot.py [e1000|e1000e|rtl8139|virtio-net]

`net_status.up` used to mean "a NIC initialised", so the menu bar's network
indicator lit the moment a card existed and stayed lit forever. A status light
that is always on is not a status light. It means CARRIER now, and this is what
checks that -- by pulling the wire out.

QEMU's `set_link` does exactly that to the virtual cable, so the whole thing is
measurable without standing next to the machine:

    link up   ->  unplug  ->  link down  ->  plug back  ->  link up

Three readings, and the middle one is the one that matters: a driver that
always answers "up" passes the first and third and fails only here. That is
precisely the driver this test exists to catch, and it is the shape the code
had before the `link` op was added.

rtl8139's bit is worth the trouble on its own: MSR bit 2 is LINKB -- "link
BAD" -- so the sense is inverted and a datasheet read too quickly ships it
backwards. This run is what settled it.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

# The menu bar's network glyph: a dot when there is a link, a dash when there is
# not. Small band, because everything around it (the CPU readout, the clock) is
# changing on its own and a wide one would report every second as a difference.
INDICATOR = (730, 4, 756, 22)


def ink(path, band, thresh=110):
    x0, y0, x1, y1 = band
    w, h, px = S.ppm_pixels(path)
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            o = (y * w + x) * 3
            if px[o] > thresh and px[o + 1] > thresh and px[o + 2] > thresh:
                n += 1
    return n


def main():
    nic = sys.argv[1] if len(sys.argv) > 1 else "e1000"
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("netlink_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("netlink", os.path.join(A.BUILD, "shot-netlink.img"), nic=nic)
    fails = []
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        buf = bytearray()

        def drain(seconds):
            s.settimeout(0.3)
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass

        def read_link(tag):
            buf.clear()
            s.sendall(b"test netlink\n")
            drain(4)
            text = buf.decode("utf-8", "replace")
            m = re.findall(r"card=(\S+) link=(\S+)", text)
            if not m:
                fails.append("%s: the kernel never answered `test netlink`" % tag)
                return None, None
            card, link = m[-1]
            print("netlink_shot: %-12s card=%-10s link=%s" % (tag, card, link))
            return card, link

        drain(22)

        card, up1 = read_link("plugged in")
        if card and card != nic and not (nic == "e1000e" and card == "e1000"):
            fails.append("expected the %s driver, got %s" % (nic, card))

        shot_in = os.path.join(A.BUILD, "shot-netlink-in.ppm")
        shot_out = os.path.join(A.BUILD, "shot-netlink-out.ppm")
        q.screendump(shot_in)

        q.cmd("set_link", name="net0", up=False)
        time.sleep(3)
        _, down = read_link("unplugged")
        q.screendump(shot_out)

        q.cmd("set_link", name="net0", up=True)
        time.sleep(2)
        _, up2 = read_link("plugged back")

        if up1 == "unknown":
            print("netlink_shot: this card cannot report carrier -- nothing to check")
        else:
            if up1 != "up":
                fails.append("link was not up with the cable in (%s)" % up1)
            if down != "down":
                fails.append("THE UNPLUG WAS NOT NOTICED: link read %s" % down)
            if up2 != "up":
                fails.append("link did not come back after plugging in (%s)" % up2)

            # AND THE MENU BAR. The kernel knowing is half of it; the point of
            # knowing is that the indicator stops claiming to be online.
            a, b = ink(shot_in, INDICATOR), ink(shot_out, INDICATOR)
            print("netlink_shot: menu-bar indicator %d lit pixels with the cable "
                  "in, %d with it out" % (a, b))
            if a == b:
                fails.append("the menu bar drew the same thing either way (%d) -- "
                             "the indicator is not reading carrier" % a)
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== netlink-shot (%s): %s" % (nic, "FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
