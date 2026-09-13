#!/usr/bin/env python3
"""menubar_shot.py -- do the menu bar's preferences reach the menu bar?

    python3 tools/menubar_shot.py

THE THING UNDER TEST IS A DELIVERY ROUTE, not a widget. The menu bar runs with
`ro /system, rw /run` and its manifest says so on purpose -- it cannot open the
user's settings file and should not be given the authority to. So the desktop
PUBLISHES the effective preferences into /run and the bar reads that copy.

Three processes, two files, and nothing in the type system to keep them in
agreement: Settings writes ~/settings.conf, the desktop copies it to
/run/prefs.conf, the bar reads /run/prefs.conf. Every one of those links has
been silently broken in this repo at some point, so the only claim worth making
is the end-to-end one.

HOW IT JUDGES, without asking a clock to hold still. It turns OFF "Show
processor load" and checks that the processor readout's patch of the bar goes
from inked to empty. Presence and absence, in a region nothing else occupies --
where a clock comparison would have to survive the digits changing underneath
it, and a width comparison would have to survive every neighbour's width too.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

DOCK_SLOTS = 4                          # Files, Terminal, Settings, Vellum --
                                        # the dock drops entries whose program is
                                        # not installed, so this is not "however
                                        # many are in the table"
SETTINGS_SLOT = 2
PANE_DESKTOP = (181, 141)               # "Desktop & Dock" in the sidebar
CPU_TOGGLE = (870, 469)                 # the "Show processor load" switch
CPU_BAND = (782, 4, 876, 22)            # x0, y0, x1, y1 -- the meter and its text
MIN_INK = 40                            # lit pixels: the readout is far denser


def ink(path, band, thresh=120):
    """How many pixels in `band` are clearly brighter than the wallpaper behind
    the bar -- i.e. how much text and meter is drawn there."""
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
        print("menubar_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("menubar", os.path.join(A.BUILD, "shot-menubar.img"))
    fails = []
    try:
        A.connect(ser)
        q = A.Qmp(qmp)
        time.sleep(24)                                  # boot + desktop

        before = os.path.join(A.BUILD, "shot-menubar-before.ppm")
        after = os.path.join(A.BUILD, "shot-menubar-after.ppm")
        q.screendump(before)

        S.move(q, S.slot_center(SETTINGS_SLOT, DOCK_SLOTS), S.dock_center_y())
        S.click(q)
        time.sleep(16)
        S.move(q, *PANE_DESKTOP)
        S.click(q)
        time.sleep(3)
        S.move(q, *CPU_TOGGLE)
        S.click(q)
        time.sleep(4)                                   # publish + the bar's 1 s poll
        q.screendump(after)

        a = ink(before, CPU_BAND)
        b = ink(after, CPU_BAND)
        print("menubar_shot: processor readout %d lit pixels, then %d" % (a, b))
        if a < MIN_INK:
            fails.append("the processor readout was not there to begin with (%d lit "
                         "pixels) -- the test is aimed at the wrong place" % a)
        elif b > a // 4:
            fails.append("the readout is still drawn after switching it off (%d -> %d "
                         "lit pixels) -- the preference never reached the menu bar"
                         % (a, b))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== menubar-shot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
