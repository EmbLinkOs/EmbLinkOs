#!/usr/bin/env python3
"""snap_shot.py -- can the machine photograph its own screen, without stopping?

    python3 tools/snap_shot.py

TWO CLAIMS, and the second is the one that needed the design.

  1. "Take Screenshot" on the desktop menu writes a PPM into Pictures. The
     shell says so on the serial line when the file is actually on disk --
     not when the work was started, which is a different and much easier
     thing to be true.

  2. THE DESKTOP KEEPS DRAWING WHILE IT WRITES. EMBKFS commits as it goes and
     two and a half megabytes takes it the better part of a minute; doing that
     on the render thread would freeze the whole desktop for that long. The
     capture is instantaneous and the write is not, so they run on different
     threads, and this checks that they really do.

Claim 2 is checked by HOVERING A DOCK ICON while the write is in flight. The
dock's hover highlight is drawn by the desktop itself, so it appears only if
that process is still rendering -- a frozen desktop shows the dock exactly as
it was. (The first version of this test watched the clock instead, which was
useless: it shows hours and minutes, so six seconds apart it is *supposed* to
be identical, and the test reported a freeze on a perfectly live machine.)
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

DESKTOP_POINT = (700, 400)              # empty wallpaper, right of the icons
MENU_ITEM = (774, 518)                  # "Take Screenshot" in the desktop menu
DOCK_BAND = (390, 686, 640, 750)        # the dock pill, where hover is drawn


def ink(path, band, thresh=120):
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
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("snap_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("snap", os.path.join(A.BUILD, "shot-snaprun.img"))
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

        drain(24)

        S.move(q, *DESKTOP_POINT)
        q.cmd("input-send-event", events=[{"type": "btn",
              "data": {"down": True, "button": "right"}}])
        time.sleep(0.2)
        q.cmd("input-send-event", events=[{"type": "btn",
              "data": {"down": False, "button": "right"}}])
        time.sleep(1.5)
        S.move(q, *MENU_ITEM)
        S.click(q)

        # Is the DESKTOP still rendering while it writes? Hover a dock icon: the
        # highlight behind it is drawn by the desktop, so it appears only if
        # that process is still running its loop.
        time.sleep(3)
        a = os.path.join(A.BUILD, "shot-snap-live-a.ppm")
        b = os.path.join(A.BUILD, "shot-snap-live-b.ppm")
        q.screendump(a)
        S.move(q, S.slot_center(1, 4), S.dock_center_y())
        time.sleep(3)
        q.screendump(b)
        ia, ib = ink(a, DOCK_BAND), ink(b, DOCK_BAND)
        print("snap_shot: dock band %d lit pixels, %d with the pointer on an icon"
              % (ia, ib))
        if abs(ia - ib) < 30:
            fails.append("the dock did not react to the pointer -- the desktop is "
                         "frozen while the screenshot is written (%d vs %d)" % (ia, ib))

        drain(75)
        text = buf.decode("utf-8", "replace")
        open(os.path.join(A.BUILD, "snap-serial.log"), "w").write(text)
        if "home: screenshot saved ->" in text:
            line = [l for l in text.splitlines() if "home: screenshot" in l][-1]
            print("snap_shot: %s" % line.strip())
        else:
            fails.append("the shell never reported a saved screenshot")
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== snap-shot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
