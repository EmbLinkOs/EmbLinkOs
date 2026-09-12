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
import json, os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A

SCREEN_W, SCREEN_H = 1024, 768
DOCK_SIZE = 38                      # oscfg default; home.c's DOCK_BASE
SLOT_W    = DOCK_SIZE + 8
# Control-cluster segment centres of a freshly opened EmUI window, measured off
# a screendump. Order is minimize, maximize, close (ui/dsl/em.c).
MIN_BTN = (222, 193)
MAX_BTN = (247, 193)
CLOSE_BTN = (271, 193)
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


def move(q, x, y):
    """Put the pointer on (x, y), with the PS/2 mouse QEMU gives every PC.

    RELATIVE, because that is the only pointer this guest has. An absolute
    device would be better and the kernel can drive one -- usb_tablet_process_
    report() feeds mouse_set_absolute() -- but attaching QEMU's usb-tablet on a
    qemu-xhci controller moved the cursor NOT AT ALL: it stayed at its boot
    position (516, 390) through the whole run while every click landed on
    nothing. Recorded in docs/TODO.md; a USB mouse is not optional on a real
    machine.

    So: home into the top-left corner, where the driver clamps (mouse.c), then
    step out to the target. MANY SMALL PACKETS rather than few large ones --
    the first version sent 12 of (-120,-120) and then hops of 100, and landed
    ninety pixels short of a 24px button. A PS/2 packet carries one signed byte
    per axis through a one-byte controller buffer, and a dropped one is silent;
    small steps make each one cheap and the paced sleeps give the guest time to
    take them."""
    for _ in range(30):
        q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": -60}},
            {"type": "rel", "data": {"axis": "y", "value": -60}}])
        time.sleep(0.02)
    time.sleep(0.4)
    dx, dy = int(x), int(y)
    while dx > 0 or dy > 0:
        sx, sy = min(dx, 40), min(dy, 40)
        q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}])
        dx -= sx; dy -= sy
        time.sleep(0.02)
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
        # Wait for the desktop, by watching the serial console for the shell's
        # own prompt rather than by sleeping a guessed number of seconds.
        s.settimeout(180)
        buf = b""
        while b"first frame presented" not in buf and b"desktop ready" not in buf:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
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

        move(q, MAX_BTN[0], MAX_BTN[1]); click(q)
        time.sleep(3)
        move(q, SCREEN_W / 2.0, SCREEN_H - 200)
        time.sleep(1.5)
        maxed = os.path.join(A.BUILD, "shell-maximized.ppm")
        q.screendump(maxed)
        f3 = A.changed_fraction(open(back, "rb").read(), open(maxed, "rb").read())
        ok_max = f3 > 0.05
        print("shell_shot: maximize changed %.1f%% of the screen %s"
              % (f3 * 100.0, "OK" if ok_max else "-- NOTHING HAPPENED"))

        return 0 if (ok_min and ok_res and ok_max) else 1
    finally:
        p.kill()


if __name__ == "__main__":
    sys.exit(main())
