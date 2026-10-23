#!/usr/bin/env python3
"""shell_shot.py -- photograph the SHELL, driven the way a person drives it.

    python3 tools/shell_shot.py            # dock click + hover, three shots

app_shot.py launches an app by typing at the serial console, which is the
right tool for "does this app draw". It is the wrong tool for the DOCK: an app
started from the shell was never started by the dock, so the dock's own record
of what is running -- the thing its sockets report -- stays empty, and a
screenshot of that proves nothing about the dock at all.

So this one clicks. It boots the desktop, moves the guest's tablet to a dock
slot, presses and releases, waits for the app, and photographs: the dock at
rest, the dock with the pointer on a slot (the hover plate and the readout),
and the dock with that app RUNNING (its socket lit).

The slot arithmetic is the dock's own, from home.c: the rack is centred, each
slot is dock_size+8 wide with 10px gaps and 12px of padding at each end.
"""
import json, os, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A

SCREEN_W, SCREEN_H = 1024, 768
DOCK_SIZE = 38                      # oscfg default; home.c's DOCK_BASE
SLOT_W    = DOCK_SIZE + 8
# Control-cluster segment centres of a freshly opened EmUI window, measured off
# a screendump. Order is minimize, maximize, close (ui/dsl/em.c).
# The three lights of a freshly opened EmUI window, MEASURED off a screendump
# (the dots span x=216..291 at y=191). 13px dots in 24px hit cases, and the
# bar's own 8px spacing between them, so the pitch is 32.
CLOSE_BTN = (222, 193)
MIN_BTN   = (254, 193)
ZOOM_BTN  = (286, 193)
# The same light once the window has been FILLED (it moves to 0,32) and once it
# is a LEFT half -- the lights ride the window, so the board's anchor does too.
ZOOM_BTN_FILLED = (86, 57)
ZOOM_BTN_HALF   = (86, 57)
# Board tiles, measured off shell-zoomboard.ppm: three columns 60px apart, two
# rows 60px apart, anchored under the light.
TILE_LEFT      = (102, 101)
TILE_FULL_HALF = (222, 161)
PITCH     = SLOT_W + 10
PAD       = 12


def slot_center(i, n):
    """Screen x of slot i, for a rack of n slots centred on the display."""
    rack_w = 2 * PAD + n * SLOT_W + (n - 1) * 10
    x0 = (SCREEN_W - rack_w) / 2.0
    return x0 + PAD + i * PITCH + SLOT_W / 2.0


def dock_center_y():
    """The rack's vertical middle: it sits a 14px gap off the bottom, and is
    dock_size+32 tall."""
    pill_h = DOCK_SIZE + 32
    return SCREEN_H - 14 - pill_h / 2.0


def ppm_pixels(path):
    """(width, height, rgb bytes) of a P6 screendump."""
    d = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j]); i = j
    i += 1
    return int(parts[1]), int(parts[2]), d[i:]


def region_changed(a_path, b_path, x0, y0, x1, y1):
    """Fraction of pixels that differ inside one rectangle of the screen.

    A whole-screen diff cannot answer "did the window cover the TOP BAR", which
    is the entire difference between the two full-screen placements this shell
    offers. This can."""
    w, h, a = ppm_pixels(a_path)
    _, _, b = ppm_pixels(b_path)
    diff = tot = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w), 3):
            o = (y * w + x) * 3
            tot += 1
            if (abs(a[o] - b[o]) > 8 or abs(a[o + 1] - b[o + 1]) > 8
                    or abs(a[o + 2] - b[o + 2]) > 8):
                diff += 1
    return diff / float(tot or 1)


_pos = [None, None]          # where we believe the guest's pointer is


def home(q):
    """Shove the pointer into the top-left corner, where the driver clamps
    (mouse.c), and call that (0, 0). The only way to establish a known position
    with a relative device."""
    for _ in range(30):
        q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": -60}},
            {"type": "rel", "data": {"axis": "y", "value": -60}}])
        time.sleep(0.02)
    _pos[0], _pos[1] = 0, 0
    time.sleep(0.4)


def move(q, x, y):
    """Put the pointer on (x, y) with the PS/2 mouse -- the only pointer this
    guest has. (QEMU's usb-tablet, which the kernel can drive absolutely, moved
    nothing at all here: docs/TODO.md.)

    IT DOES NOT RE-HOME ON EVERY MOVE, and that is not an optimisation. The
    first version homed through the top-left corner before every move, which
    walks the pointer ACROSS THE WHOLE SCREEN to get anywhere -- and a hover
    menu closes when the pointer leaves it. Every click on the zoom board
    therefore arrived after the board had dismissed itself, and the trace said
    exactly that: the board rendered, and no click ever reached it. The bug was
    in the aiming, and it looked precisely like a dead widget.

    So: home once, then track. Small steps and paced sleeps because a PS/2
    packet carries one signed byte per axis through a one-byte controller
    buffer, and a dropped one is silent."""
    if _pos[0] is None:
        home(q)
    dx, dy = int(x) - _pos[0], int(y) - _pos[1]
    while dx or dy:
        sx = max(-40, min(40, dx))
        sy = max(-40, min(40, dy))
        q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}])
        dx -= sx; dy -= sy
        time.sleep(0.02)
    _pos[0], _pos[1] = int(x), int(y)
    time.sleep(0.4)


def click(q):
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.2)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])


def main():
    name = "shell"
    # EXACT executable name, not a command-line substring. `pgrep -f
    # qemu-system` matches ANY process whose command line contains that text --
    # including a shell one-liner that waits for qemu to exit, which is exactly
    # what a person or a script babysitting a long run will be running. It cost
    # a real debugging session: every tool here refused to start, reporting
    # "another qemu-system is running", while nothing was running but the
    # waiters themselves. -x matches the program, which is the question.
    if any(subprocess.run(["pgrep", "-x", exe], capture_output=True).returncode == 0
           for exe in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("shell_shot: another qemu-system is running; refusing to start a second one")
        return 2
    p, ser, qmp = A.boot(name, os.path.join(A.BUILD, "shot-%s.img" % name))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        # KEEP DRAINING THE SERIAL to a file. The guest's apps write to fd 1,
        # which is this console, and that is the only channel a GUI app has for
        # saying what it did -- its printf goes nowhere near the screen. A
        # test that photographs the result but throws away the commentary can
        # tell you something did not happen and never why.
        serial_log = open(os.path.join(A.BUILD, "shell-serial.log"), "wb")
        def drain():
            while True:
                try:
                    b = s.recv(4096)
                except OSError:
                    return
                if not b:
                    return
                serial_log.write(b); serial_log.flush()
        threading.Thread(target=drain, daemon=True).start()
        # Wait for the desktop, by watching the serial console for the shell's
        # own prompt rather than by sleeping a guessed number of seconds.
        t0 = time.time()
        while time.time() - t0 < 180:
            try:
                seen = open(os.path.join(A.BUILD, "shell-serial.log"), "rb").read()
            except OSError:
                seen = b""
            if b"first frame presented" in seen or b"desktop ready" in seen:
                break
            time.sleep(1)
        time.sleep(8)                       # let the dock settle its first frames

        q.screendump(os.path.join(A.BUILD, "shell-rest.ppm"))
        print("shell_shot: rest")

        slot = int(os.environ.get("SLOT", "1"))     # 1 = the terminal
        n    = int(os.environ.get("SLOTS", "5"))
        x, y = slot_center(slot, n), dock_center_y()
        move(q, x, y)
        time.sleep(1.5)
        q.screendump(os.path.join(A.BUILD, "shell-hover.ppm"))
        print("shell_shot: hover at slot %d (%.0f,%.0f)" % (slot, x, y))

        click(q)
        time.sleep(12)                      # the app has to start and present
        move(q, SCREEN_W / 2.0, SCREEN_H / 2.0 - 120)   # pointer off the dock
        time.sleep(1.5)
        run_shot = os.path.join(A.BUILD, "shell-running.ppm")
        q.screendump(run_shot)
        print("shell_shot: running")

        # --- the window controls, end to end ---------------------------
        # MINIMIZE FIRST, THEN MAXIMIZE, and the order is not a preference: a
        # maximized window moves to (0,32), so a cluster coordinate measured on
        # the window where it opened points into the middle of the maximized
        # window's content. The first version of this ran maximize first and
        # reported "minimize: NOTHING HAPPENED" -- which was true of the click
        # and false of the button.
        #
        # These are the segment centres MEASURED off shell-running.ppm. The
        # checks do not trust them anyway: each compares the pixels before and
        # after, so a click that misses is a failure, not a screenshot of
        # nothing happening.
        before = open(run_shot, "rb").read()

        # The close segment under the pointer, photographed BEFORE anything is
        # clicked: the design claims colour is spent only on the segment being
        # pointed at, and that claim is worth a picture.
        move(q, CLOSE_BTN[0], CLOSE_BTN[1])
        time.sleep(1.5)
        q.screendump(os.path.join(A.BUILD, "shell-ctlhover.ppm"))
        print("shell_shot: control hover")

        # The zoom light's board: resting on the light opens it. Photographed
        # before anything is clicked, because "a board appears" is the claim.
        move(q, ZOOM_BTN[0], ZOOM_BTN[1])
        time.sleep(2)
        board = os.path.join(A.BUILD, "shell-zoomboard.ppm")
        q.screendump(board)
        f0 = A.changed_fraction(open(run_shot, "rb").read(), open(board, "rb").read())
        print("shell_shot: zoom board changed %.1f%% of the screen %s"
              % (f0 * 100.0, "OK" if f0 > 0.005 else "-- NOTHING OPENED"))

        move(q, MIN_BTN[0], MIN_BTN[1]); click(q)
        time.sleep(3)
        move(q, SCREEN_W / 2.0, SCREEN_H / 2.0 - 200)
        time.sleep(1.5)
        parked = os.path.join(A.BUILD, "shell-minimized.ppm")
        q.screendump(parked)
        f = A.changed_fraction(before, open(parked, "rb").read())
        ok_min = f > 0.05
        print("shell_shot: minimize changed %.1f%% of the screen %s"
              % (f * 100.0, "OK" if ok_min else "-- NOTHING HAPPENED"))

        # and back, from the dock -- the only way a parked window returns
        move(q, x, y); click(q)
        time.sleep(4)
        move(q, SCREEN_W / 2.0, SCREEN_H / 2.0 - 200)
        time.sleep(1.5)
        back = os.path.join(A.BUILD, "shell-restored.ppm")
        q.screendump(back)
        f2 = A.changed_fraction(open(parked, "rb").read(), open(back, "rb").read())
        ok_res = f2 > 0.05
        print("shell_shot: restore changed %.1f%% of the screen %s"
              % (f2 * 100.0, "OK" if ok_res else "-- NOTHING HAPPENED"))

        move(q, ZOOM_BTN[0], ZOOM_BTN[1]); click(q)
        time.sleep(3)
        move(q, SCREEN_W / 2.0, SCREEN_H - 200)
        time.sleep(1.5)
        maxed = os.path.join(A.BUILD, "shell-maximized.ppm")
        q.screendump(maxed)
        f3 = A.changed_fraction(open(back, "rb").read(), open(maxed, "rb").read())
        ok_max = f3 > 0.05
        print("shell_shot: zoom click (fill) changed %.1f%% of the screen %s"
              % (f3 * 100.0, "OK" if ok_max else "-- NOTHING HAPPENED"))

        # --- the board's placements ------------------------------------
        # LEFT: the window takes the left half, so the RIGHT half of the work
        # area goes back to being desktop. Checked as a region, because a
        # whole-screen diff cannot tell "moved" from "moved where".
        move(q, ZOOM_BTN_FILLED[0], ZOOM_BTN_FILLED[1])
        time.sleep(2)
        move(q, TILE_LEFT[0], TILE_LEFT[1]); click(q)
        time.sleep(3)
        move(q, SCREEN_W / 2.0, SCREEN_H - 120)
        time.sleep(1.5)
        half = os.path.join(A.BUILD, "shell-left.ppm")
        q.screendump(half)
        fr = region_changed(maxed, half, SCREEN_W // 2 + 40, 120, SCREEN_W - 20, 500)
        ok_left = fr > 0.20
        print("shell_shot: Left -- right half of the screen changed %.1f%% %s"
              % (fr * 100.0, "OK" if ok_left else "-- THE WINDOW DID NOT MOVE"))

        # FULL: the one that covers the top bar. That is the whole difference
        # between it and Fill, so that is what gets measured -- the bar's own
        # strip, not the screen at large.
        move(q, ZOOM_BTN_HALF[0], ZOOM_BTN_HALF[1])
        time.sleep(2)
        move(q, TILE_FULL_HALF[0], TILE_FULL_HALF[1]); click(q)
        time.sleep(3)
        move(q, SCREEN_W / 2.0, SCREEN_H / 2.0)
        time.sleep(1.5)
        full = os.path.join(A.BUILD, "shell-full.ppm")
        q.screendump(full)
        fb = region_changed(half, full, 0, 0, SCREEN_W, 24)
        ok_full = fb > 0.10
        print("shell_shot: Full -- the top bar strip changed %.1f%% %s"
              % (fb * 100.0, "OK" if ok_full else "-- THE BAR IS STILL THERE"))

        return 0 if (ok_min and ok_res and ok_max and ok_left and ok_full) else 1
    finally:
        p.kill()


if __name__ == "__main__":
    sys.exit(main())
