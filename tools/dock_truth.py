#!/usr/bin/env python3
"""dock_truth.py -- does the dock light a tile for an app IT did not launch?

    python3 tools/dock_truth.py

THE CLAIM UNDER TEST. The dock used to light a socket from the desktop's own
record of what it had spawned (`g_running[]` in home.c, keyed by spawn handle),
so an app started any other way -- from the shell, by another app -- left its
tile dark while its window sat in plain view. With a window list the dock can
stop guessing, and this measures whether it did.

HOW IT LAUNCHES SOMETHING WITHOUT THE DOCK, which is the whole difficulty. It
cannot use the serial console: a program started with `run` at the KERNEL
console is a child of the kernel and lives in session 0, and the window list is
scoped to the caller's session, so the desktop correctly never sees it. So this
clicks the dock's Terminal tile, and types into the TERMINAL -- whose shell is
a grandchild of the desktop and therefore in the user's session, exactly like
an app the person started for themselves.

WHAT IT MEASURES: each tile against ITSELF, across three photographs.

    A  nothing launched
    B  the dock has launched the Terminal
    C  the TERMINAL's shell has launched Settings

    Terminal (slot 1)  must change A->B   the dock lights what it launched,
                                          and the coordinates are on the dock
    Settings (slot 2)  must hold A->B     nothing has started it yet
                       must change B->C   THE CLAIM: lit for an app the dock
                                          did not launch
    NetSurf  (slot 4)  must hold all      nothing is drifting under the dock

NOT TILE AGAINST TILE, which is what the first version of this did and why it
reported a failure that was not one. Each socket holds a different ICON, so
their mean colours differ by more than lighting one ever moves them: the
NetSurf tile reads (74,77,84) and the Settings tile (34,43,67) with neither
app running. A tile is only comparable with itself.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

SCREEN_W, SCREEN_H = 1024, 768
SLOTS = 5
TERM_SLOT, SET_SLOT, DARK_SLOT = 1, 2, 4


def tile_rgb(path, slot):
    """Mean colour of one dock socket. The plate is the tile, so a lit one and
    a dark one differ across the whole square -- no need to find an edge."""
    w, h, px = S.ppm_pixels(path)
    cx = int(S.slot_center(slot, SLOTS))
    cy = int(S.dock_center_y())
    r = g = b = n = 0
    for y in range(cy - 20, cy + 21):
        for x in range(cx - 20, cx + 21):
            o = (y * w + x) * 3
            r += px[o]; g += px[o + 1]; b += px[o + 2]; n += 1
    return (r / n, g / n, b / n)


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def type_text(q, text):
    """Type at the GUEST's keyboard, not the serial console -- the terminal
    window reads the real keyboard, and the point of this test is to be a
    person at the machine."""
    NAMES = {"/": "slash", ".": "dot", "\n": "ret", " ": "spc", "-": "minus"}
    for ch in text:
        k = NAMES.get(ch, ch)
        q.cmd("input-send-event", events=[
            {"type": "key", "data": {"down": True,
                                     "key": {"type": "qcode", "data": k}}}])
        time.sleep(0.04)
        q.cmd("input-send-event", events=[
            {"type": "key", "data": {"down": False,
                                     "key": {"type": "qcode", "data": k}}}])
        time.sleep(0.08)


def main():
    if any(subprocess.run(["pgrep", "-x", exe], capture_output=True).returncode == 0
           for exe in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("dock_truth: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("docktruth", os.path.join(A.BUILD, "shot-docktruth.img"))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "docktruth-serial.log"), "wb")

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
        while time.time() - t0 < 180:
            seen = open(log.name, "rb").read()
            if b"first frame presented" in seen or b"desktop ready" in seen:
                break
            time.sleep(1)
        time.sleep(8)

        # A. The dock at rest. Nothing has been launched.
        x, y = S.slot_center(TERM_SLOT, SLOTS), S.dock_center_y()
        S.move(q, SCREEN_W / 2.0, 300)          # pointer off the dock: no hover plate
        time.sleep(2)
        a_shot = os.path.join(A.BUILD, "dock-rest.ppm")
        q.screendump(a_shot)

        # B. The dock launches the Terminal. This half already worked.
        S.move(q, x, y)
        S.click(q)
        time.sleep(14)
        S.move(q, SCREEN_W / 2.0, 300)
        time.sleep(2)
        b_shot = os.path.join(A.BUILD, "dock-term.ppm")
        q.screendump(b_shot)

        # C. The TERMINAL's shell launches Settings -- the dock is not involved.
        type_text(q, "settings\n")
        time.sleep(16)
        S.move(q, SCREEN_W / 2.0, 300)
        time.sleep(2)
        c_shot = os.path.join(A.BUILD, "dock-shelllaunch.ppm")
        q.screendump(c_shot)

        shots = [("rest ", a_shot), ("term ", b_shot), ("shell", c_shot)]
        tiles = {}
        for tag, path in shots:
            tiles[tag] = t = tuple(tile_rgb(path, i) for i in (TERM_SLOT, SET_SLOT, DARK_SLOT))
            print("dock_truth: %s  term=%s settings=%s netsurf=%s"
                  % ((tag,) + tuple("(%3.0f,%3.0f,%3.0f)" % c for c in t)))

        (at, as_, ad) = tiles["rest "]
        (bt, bs, bd) = tiles["term "]
        (ct, cs, cd) = tiles["shell"]
        LIT, SAME = 20, 12       # a lit socket moves a tile far further than noise

        fails = []
        # THE AIM IS ON THE DOCK AT ALL. Without this the whole run could be
        # photographing wallpaper and reporting whatever it liked.
        if dist(at, bt) < LIT:
            fails.append("the Terminal tile did not light for the dock's OWN launch "
                         "(%d) -- the aim or the dock is wrong" % dist(at, bt))
        if dist(as_, bs) > SAME:
            fails.append("the Settings tile changed before anything started it (%d)"
                         % dist(as_, bs))
        # THE CLAIM.
        if dist(bs, cs) < LIT:
            fails.append("the Settings tile stayed DARK for an app the dock did not "
                         "launch (%d)" % dist(bs, cs))
        # And nothing is quietly moving under the dock.
        if dist(ad, bd) > SAME or dist(bd, cd) > SAME:
            fails.append("the never-launched tile changed -- something moved under the dock")

        for f in fails:
            print("dock_truth: FAIL %s" % f)
        print("dock_truth: dock-lights-own %d  settings-before %d  "
              "settings-after %d  reference-drift %d"
              % (dist(at, bt), dist(as_, bs), dist(bs, cs), max(dist(ad, bd), dist(bd, cd))))
        print("dock_truth: %s" % ("OK" if not fails else "FAILED"))
        return 1 if fails else 0
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
