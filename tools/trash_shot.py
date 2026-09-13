#!/usr/bin/env python3
"""trash_shot.py -- does Delete put the file somewhere you can get it back from?

    python3 tools/trash_shot.py

Files had a Trash in its sidebar for a long time and nothing ever went into it:
Delete called unlink, and the confirmation sheet said so -- "there is no Trash
yet, so the file is gone for good". Honest, and still the wrong behaviour for
the verb people reach for most carelessly.

THE ROUND TRIP, driven with a real pointer:

    move readme.txt to the Trash  ->  it is there
                                  ->  Put Back  ->  it is not

It judges by ink in the FIRST GRID CELL, which is empty unless something is
listed. Measuring the whole listing area was the obvious idea and was wrong:
an empty folder draws a large "Empty folder" placeholder in the middle of it,
which inks MORE pixels than one small file icon does -- so "more ink" and "has
a file" are not the same thing, and the first version of this test could not
tell 0 items from 1.

The last leg is the one that separates a trash from a delete. Moving a file
somewhere it can never come back from is not a trash, it is a slower unlink --
so the test does not stop at "it vanished from home".
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

DOCK_SLOTS, FILES_SLOT = 4, 0
SIDEBAR_TRASH = (80, 417)
SIDEBAR_HOME = (76, 163)
FILE_ICON = (410, 293)                  # readme.txt, in the home grid
FIRST_CELL = (303, 205)                 # the first grid cell, wherever you are
CELL = (270, 180, 350, 245)             # the first grid cell, and nothing else

# The item menu opens at the pointer: first row +19, then 33 apart.
def menu_row(click_xy, i):
    return (click_xy[0] + 70, click_xy[1] + 19 + 33 * i)

CONFIRM_BUTTON = (611, 414)             # the sheet's destructive button


def ink(path, band, thresh=95):
    x0, y0, x1, y1 = band
    w, h, px = S.ppm_pixels(path)
    n = 0
    for y in range(y0, min(y1, h), 2):
        for x in range(x0, min(x1, w), 2):
            o = (y * w + x) * 3
            if px[o] > thresh and px[o + 1] > thresh and px[o + 2] > thresh:
                n += 1
    return n


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("trash_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("trash", os.path.join(A.BUILD, "shot-trash.img"))
    fails = []
    try:
        sock = A.connect(ser)
        q = A.Qmp(qmp)
        buf = bytearray()

        def drain(seconds):
            sock.settimeout(0.3)
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = sock.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass

        drain(22)

        S.move(q, S.slot_center(FILES_SLOT, DOCK_SLOTS), S.dock_center_y())
        S.click(q)
        drain(16)

        def shot(name):
            path = os.path.join(A.BUILD, "shot-trash-%s.ppm" % name)
            q.screendump(path)
            return path

        def right_click(xy):
            S.move(q, *xy)
            q.cmd("input-send-event", events=[{"type": "btn",
                  "data": {"down": True, "button": "right"}}])
            time.sleep(0.2)
            q.cmd("input-send-event", events=[{"type": "btn",
                  "data": {"down": False, "button": "right"}}])
            time.sleep(1.5)

        # ONE NAVIGATION, deliberately. Files' grid shifts down 32px and its
        # rows spread 33px apart on the SECOND navigation of a session (see
        # docs/TODO.md) -- so a test that wandered around before clicking would
        # aim at coordinates that no longer hold and report the Trash broken
        # when the bug is somewhere else entirely.
        #
        # 1. move a file to the Trash, from the view Files opens on
        right_click(FILE_ICON)
        S.move(q, *menu_row(FILE_ICON, 3)); S.click(q)     # "Move to Trash..."
        time.sleep(1.5)
        S.move(q, *CONFIRM_BUTTON); S.click(q)
        time.sleep(2)

        # 2. it is in the Trash
        S.move(q, *SIDEBAR_TRASH); S.click(q); time.sleep(2)
        filled = ink(shot("filled"), CELL)
        print("trash_shot: Trash's first cell has %d lit pixels after a delete" % filled)
        if filled < 30:
            fails.append("nothing appeared in the Trash (first cell %d lit pixels) "
                         "-- Delete did not move the file there" % filled)

        # 3. put it back, and the Trash is empty again
        right_click(FIRST_CELL)
        S.move(q, *menu_row(FIRST_CELL, 3)); S.click(q)    # "Put Back"
        time.sleep(2)
        again = ink(shot("back"), CELL)
        print("trash_shot: %d lit pixels after Put Back" % again)
        if again > 30:
            fails.append("the item is still in the Trash after Put Back (first cell "
                         "%d lit pixels)" % again)

        drain(2)
        text = buf.decode("utf-8", "replace")
        open(os.path.join(A.BUILD, "trash-serial.log"), "w").write(text)
        for line in text.splitlines():
            if "files: trash" in line:
                print("trash_shot: %s" % line.strip())
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== trash-shot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
