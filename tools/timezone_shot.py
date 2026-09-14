#!/usr/bin/env python3
"""tools/timezone_shot.py -- the time zone, from the picker to the desktop.

THE THING UNDER TEST IS A DELIVERY ROUTE, not a widget. Three processes and a
file stand between "the user picked Tokyo" and "this machine shows Tokyo time":
Settings writes ~/settings.conf, the desktop re-reads it on its one-second
poll, applies the zone to itself, and hands it to every application it launches
afterwards. Nothing in the type system keeps those in agreement.

HOW IT JUDGES. Not by reading a clock off the screen -- the digits move, and
recognising them from pixels would be a second thing to get wrong. The desktop
announces the zone it has actually adopted on the console, and that line is
the end of the route: it cannot be printed unless the file was written, read
back and applied.

`make test-tz` covers the other half (libc converting a parent-supplied zone).
This covers the half a person touches.
"""
import os
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

DOCK_SLOTS = 4                  # Files, Terminal, Settings, Web -- the dock drops
SETTINGS_SLOT = 2               # entries whose program is not installed

# Measured off build/tz-*.ppm at 1024x768 with the default interface size.
PANE_DATE_TIME = (165, 185)     # the sidebar row
GROUP_ASIA     = (616, 220)     # "Asia & Pacific" in the region row
DROPDOWN       = (610, 261)     # the city field
ITEM0_Y        = 299            # the first item of the open list
ITEM_DY        = 34.3           # and the spacing between them
TOKYO          = 5              # Dubai, Karachi, Mumbai, Bangkok, Beijing, TOKYO, Sydney

WANT = "home: time zone Tokyo"  # what the desktop must say afterwards
RULE = "TZ=JST-9"               # and the rule it must have adopted


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("timezone_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("tz", os.path.join(A.BUILD, "shot-tz.img"))
    log = []
    fails = []
    try:
        s = A.connect(ser)

        def drain():
            while True:
                try:
                    d = s.recv(4096)
                except OSError:
                    return
                if not d:
                    return
                log.append(d.decode("utf-8", "replace"))

        threading.Thread(target=drain, daemon=True).start()
        q = A.Qmp(qmp)
        time.sleep(40)                                   # boot + desktop

        before = "".join(log)
        if WANT in before:
            fails.append("the desktop was already in Tokyo before anything was "
                         "picked -- the test cannot tell a change from a default")

        S.move(q, S.slot_center(SETTINGS_SLOT, DOCK_SLOTS), S.dock_center_y())
        S.click(q)
        time.sleep(18)                                   # Settings is a big app
        S.move(q, *PANE_DATE_TIME); S.click(q)
        time.sleep(3)
        S.move(q, *GROUP_ASIA);     S.click(q)
        time.sleep(3)
        S.move(q, *DROPDOWN);       S.click(q)
        time.sleep(3)
        S.move(q, DROPDOWN[0], int(ITEM0_Y + TOKYO * ITEM_DY)); S.click(q)
        time.sleep(6)                                    # save + the desktop's 1 s poll

        q.screendump(os.path.join(A.BUILD, "tz-picked.ppm"))
        after = "".join(log)[len(before):]
        line = [l for l in after.splitlines() if l.startswith("home: time zone")]
        print("timezone_shot: the desktop said: %s" % (line[-1] if line else "(nothing)"))
        if not line:
            fails.append("the desktop never announced a new zone -- the preference "
                         "did not reach it")
        elif WANT not in line[-1] or RULE not in line[-1]:
            fails.append("the desktop adopted %r, not Tokyo (%s)" % (line[-1], RULE))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== test-timezone: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
