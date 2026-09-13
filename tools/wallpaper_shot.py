#!/usr/bin/env python3
"""wallpaper_shot.py -- does picking a desktop picture in Settings do anything?

    python3 tools/wallpaper_shot.py

THE CLAIM, and why it needs a test of its own. A preference is real only when
three parties agree on it: the app that WRITES it, the shell that READS it, and
the screen that shows the result. Every one of those can be correct on its own
while the chain does nothing, and this repo has now shipped that exact failure
twice -- sys_kbd_layout rejecting every name, and Settings reading a segmented
control's value one flush too early. Both times the unit tests passed.

So this drives the real control with a real pointer and then asks the GUEST
what it wrote, rather than trusting a screenshot to mean what it looks like:

  1. launch Settings, photograph it
  2. click the "Aurora" segment of the Desktop picture control
  3. photograph it again -- the PREVIEW under the control must change, which is
     the widget -> value half of the chain
  4. read the session's settings.conf back on the guest -- it must say
     `wallpaper 3`, which is the value -> stored preference half
  5. the strip of WALLPAPER beside the window must have changed too, which is
     the stored preference -> shell half: the desktop re-reads the file about
     once a second and repaints itself

Step 4 cannot be faked by a repaint, step 3 cannot be faked by a file write,
and step 5 cannot be faked by either -- it is a different process. A run needs
all three, because each of the three parties has been the broken one before.
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

# Where the control sits, measured off a screenshot of this exact pane at
# 1024x768. app_shot centres the window, so these are stable between runs --
# and when they are not, step 4 fails with "wallpaper 0", which says the click
# missed rather than leaving a green run that proved nothing.
AURORA_X, AURORA_Y = 577, 285
PREVIEW = (332, 352, 888, 470)          # x0, y0, x1, y1 of the preview image
                                        # (below BOTH rows of choices -- a band that
                                        #  overlaps a row measures the control, not
                                        #  the preview, and passes for the wrong reason)
DESKTOP = (0, 120, 78, 620)             # the strip of wallpaper left of the window
WANT_INDEX = 3                          # oscfg_wallpapers[3] == Aurora
DOCK_SLOTS = 5
SETTINGS_SLOT = 2                       # Files, Terminal, Settings, Vellum, NetSurf


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("wallpaper_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("wallpaper", os.path.join(A.BUILD, "shot-wallpaper.img"))
    fails = []
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        buf = bytearray()
        full = bytearray()

        def drain(seconds):
            s.settimeout(0.4)
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b); full.extend(b)
                except Exception:
                    pass

        def send(line):
            s.sendall((line + "\n").encode())

        drain(22)                                   # boot + desktop

        # FROM THE DOCK, not from the kernel console. The console's `run` spawns
        # with no namespace and no HOME, so Settings started that way writes the
        # fallback path and the desktop reads the session one -- two files, and
        # a test that proves nothing about the machine anyone actually uses.
        S.move(q, S.slot_center(SETTINGS_SLOT, DOCK_SLOTS), S.dock_center_y())
        S.click(q)
        drain(16)

        before = os.path.join(A.BUILD, "shot-wallpaper-before.ppm")
        after = os.path.join(A.BUILD, "shot-wallpaper-after.ppm")
        q.screendump(before)

        S.move(q, AURORA_X, AURORA_Y)
        S.click(q)
        time.sleep(2.0)
        q.screendump(after)

        changed = S.region_changed(before, after, *PREVIEW)
        print("wallpaper_shot: preview area changed %.1f%%" % (changed * 100))
        if changed < 0.10:
            fails.append("the preview did not change -- the segmented control's "
                         "value never reached the pane (%.1f%%)" % (changed * 100))

        # THE DESKTOP, which is a different process reading the same file. Its
        # poll is about a second, so give it two before looking.
        time.sleep(2.5)
        desk = os.path.join(A.BUILD, "shot-wallpaper-desk.ppm")
        q.screendump(desk)
        dchanged = S.region_changed(before, desk, *DESKTOP)
        print("wallpaper_shot: desktop strip changed %.1f%%" % (dchanged * 100))
        if dchanged < 0.20:
            fails.append("the desktop did not repaint -- the shell is not reading "
                         "the preference live (%.1f%%)" % (dchanged * 100))

        # What the guest actually stored. This is the half a screenshot cannot
        # answer, and the half that was broken.
        buf.clear()
        send("test cfgdump")
        drain(4)
        text = buf.decode("utf-8", "replace")
        m = re.findall(r"^wallpaper\s+(\d+)\s*$", text, re.M)
        got = int(m[-1]) if m else None
        print("wallpaper_shot: the session's settings.conf says wallpaper=%s" % got)
        if got != WANT_INDEX:
            fails.append("settings.conf says wallpaper=%s, wanted %d -- the click "
                         "did not become a stored preference" % (got, WANT_INDEX))

        open(os.path.join(A.BUILD, "wallpaper-serial.log"), "w").write(
            full.decode("utf-8", "replace"))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== wallpaper-shot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
