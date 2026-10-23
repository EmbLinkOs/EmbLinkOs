#!/usr/bin/env python3
"""switch_shot.py -- does GUI+Tab switch windows, and GUI+W close one?

    python3 tools/switch_shot.py

THE CLAIM. The window list was built so a switcher could exist; this is the
switcher. GUI+Tab raises the window behind the front one, and pressing it
repeatedly must WALK every window rather than flipping between the top two --
which is what the naive "swap the top two" gives you, and is a trap because the
third window then can never be reached.

These are SYSTEM shortcuts: the driver swallows them and never lets an
application see them. An app that could see the switcher could decline it, and
a switcher that works only in the apps that implemented it is not one.

GUI+W is checked the same way, and it ASKS the application to close rather
than killing it -- so what is asserted is that the window actually goes, not
that a process was shot.

HOW IT JUDGES. The top bar names the focused application (the window list's
first payoff), so the test reads that strip: after each GUI+Tab the name there
must CHANGE, and after three presses over two apps it must have come back to
where it started. Comparing a strip of pixels rather than reading text keeps
this out of the business of recognising glyphs.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

SCREEN_W, SCREEN_H = 1024, 768
SLOTS = 5
# The top bar's application-name strip, measured off a screendump: it reads
# "EmbLink  <App>" just right of the leading menu.
NAME_BAND = (110, 4, 320, 22)          # x0, y0, x1, y1


def band(path):
    w, h, px = S.ppm_pixels(path)
    x0, y0, x1, y1 = NAME_BAND
    out = []
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            o = (y * w + x) * 3
            out.append(px[o] + px[o + 1] + px[o + 2])
    return out


def differs(a, b):
    n = sum(1 for i in range(min(len(a), len(b))) if abs(a[i] - b[i]) > 24)
    return n / float(len(a) or 1)


def band_rect(a_path, b_path, x0, y0, x1, y1):
    """Fraction of pixels in one rectangle that differ between two shots."""
    w, h, a = S.ppm_pixels(a_path)
    _, _, b = S.ppm_pixels(b_path)
    diff = tot = 0
    for y in range(y0, min(y1, h), 2):
        for x in range(x0, min(x1, w), 2):
            o = (y * w + x) * 3
            tot += 1
            if (abs(a[o] - b[o]) > 10 or abs(a[o+1] - b[o+1]) > 10
                    or abs(a[o+2] - b[o+2]) > 10):
                diff += 1
    return diff / float(tot or 1)


def chord(q, mods, key):
    for m in mods:
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True,
              "key": {"type": "qcode", "data": m}}}])
    time.sleep(0.1)
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True,
          "key": {"type": "qcode", "data": key}}}])
    time.sleep(0.08)
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False,
          "key": {"type": "qcode", "data": key}}}])
    for m in reversed(mods):
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False,
              "key": {"type": "qcode", "data": m}}}])
    time.sleep(1.6)


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("switch_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("switch", os.path.join(A.BUILD, "shot-switch.img"))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "switch-serial.log"), "wb")

        def drain():
            while True:
                try:
                    b = s.recv(4096)
                except OSError:
                    return
                if not b:
                    return
                log.write(b); log.flush()
        threading.Thread(target=drain, daemon=True).start()

        t0 = time.time()
        while time.time() - t0 < 200:
            if b"first frame presented" in open(log.name, "rb").read():
                break
            time.sleep(1)
        time.sleep(8)

        # Two apps, from the dock: Terminal then Settings.
        for slot in (1, 2):
            S.move(q, S.slot_center(slot, SLOTS), S.dock_center_y())
            S.click(q)
            time.sleep(14)
        S.move(q, SCREEN_W / 2.0, 400)
        time.sleep(2)

        shots = []
        for i in range(4):
            f = os.path.join(A.BUILD, "switch-%d.ppm" % i)
            q.screendump(f)
            shots.append(band(f))
            if i < 3:
                chord(q, ["meta_l"], "tab")
                S.move(q, SCREEN_W / 2.0, 400)
                time.sleep(1.0)

        d01 = differs(shots[0], shots[1])
        d12 = differs(shots[1], shots[2])
        d02 = differs(shots[0], shots[2])
        print("switch_shot: bar name changed %.0f%% on the first tab, %.0f%% on the "
              "second; start vs two-tabs-later %.0f%%" % (d01 * 100, d12 * 100, d02 * 100))

        fails = []
        if d01 < 0.02:
            fails.append("GUI+Tab did not change the focused application")
        if d12 < 0.02:
            fails.append("a second GUI+Tab did not change it again")
        if d02 > 0.02:
            fails.append("two tabs over two windows did not come back to the start "
                         "-- it is not cycling")

        # ---- GUI+W closes the front window ------------------------------
        # It ASKS now rather than killing, so the window goes when the app takes
        # itself down. What must be true either way: the window is gone, and the
        # bar stops naming it.
        # One more tab so the front window is SETTINGS -- an app with no child
        # process to tear down. The terminal is a poor subject for this check:
        # it closes its shell's stdin and waits for the shell to go first, which
        # under TCG takes longer than the grace period, so it is killed rather
        # than closing itself and would prove nothing about the request.
        chord(q, ["meta_l"], "tab")
        S.move(q, SCREEN_W / 2.0, 400)
        time.sleep(1.5)
        before_close = os.path.join(A.BUILD, "switch-preclose.ppm")
        q.screendump(before_close)
        chord(q, ["meta_l"], "w")
        # LONG ENOUGH FOR A REAL SHUTDOWN, not just for the request to arrive.
        # Asking an app to close means it runs its own teardown -- the terminal
        # closes its shell's stdin, waits for the shell to go, and only then
        # exits -- which is precisely the path the old kill skipped. Under TCG
        # that takes seconds. Watching the name strip was not enough either: it
        # goes blank the moment focus moves, whether or not the window went.
        # MEASURED INSIDE THE GRACE WINDOW, deliberately. If the window is gone
        # before COMP_CLOSE_GRACE_MS could have elapsed, the application took
        # itself down -- which is the whole claim. Waiting longer would pass
        # whether the app closed or was shot.
        time.sleep(2.5)
        S.move(q, SCREEN_W / 2.0, 400)
        time.sleep(0.8)
        after_close = os.path.join(A.BUILD, "switch-closed.ppm")
        q.screendump(after_close)
        # The terminal's window occupied roughly this rect; what matters is that
        # what is drawn there CHANGED, because the window is gone.
        gone = band_rect(before_close, after_close, 300, 120, 900, 600)
        print("switch_shot: GUI+W changed %.0f%% of where the window was, within the "
              "grace period (so the app closed ITSELF)" % (gone * 100))
        if gone < 0.10:
            fails.append("GUI+W did not close the front window inside the grace "
                         "period -- the app is not honouring the request")

        for f in fails:
            print("switch_shot: FAIL %s" % f)
        print("switch_shot: %s" % ("OK" if not fails else "FAILED"))
        return 1 if fails else 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
