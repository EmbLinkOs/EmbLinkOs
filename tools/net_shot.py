#!/usr/bin/env python3
"""net_shot.py -- does the bar tell the truth about the network?

    python3 tools/net_shot.py

THE CLAIM. The stack has always known whether it had a link and what its
address was (struct netif) and userspace could open sockets without ever being
able to ask -- so nothing could show it, and a machine that cannot tell you it
is offline looks broken when it is merely not plugged in.

BOTH ANSWERS, which is the whole point. A test that only ever boots without a
NIC proves nothing: an indicator hard-wired to say "offline" would pass it. So
this boots TWICE -- once with no network device and once with QEMU's user-mode
stack, which hands out 10.0.2.15 over DHCP -- and requires the bar to differ.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

# The status glyph, MEASURED off a screendump of the bar rather than guessed --
# the first attempt was 40px right of it, on the CPU meter, which opens nothing
# and looks exactly like an indicator that does not work.
GLYPH = (742, 12)


def run(with_net):
    # FORGET WHERE THE POINTER WAS. shell_shot tracks it in a module global so
    # it can move relatively without re-homing across the screen; across two
    # separate GUESTS that position is a fiction, and the second run aimed by a
    # delta from a machine that no longer exists. Resetting it forces a re-home.
    S._pos[0] = S._pos[1] = None
    os.environ["NET"] = "1" if with_net else "0"
    tag = "net" if with_net else "nonet"
    p, ser, qmp = A.boot(tag, os.path.join(A.BUILD, "shot-%s.img" % tag))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "%s-serial.log" % tag), "wb")

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
        while time.time() - t0 < 220:
            if b"first frame presented" in open(log.name, "rb").read():
                break
            time.sleep(1)
        time.sleep(10)                     # let DHCP finish if there is a NIC

        S.move(q, *GLYPH)
        S.click(q)
        time.sleep(2.5)
        shot = os.path.join(A.BUILD, "%s-menu.ppm" % tag)
        q.screendump(shot)
        return shot
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()


def ink_area(path, x0, y0, x1, y1):
    """How much is DRAWN in a rectangle -- the menu's size, in effect."""
    w, h, px = S.ppm_pixels(path)
    lit = tot = 0
    for y in range(y0, min(y1, h), 2):
        for x in range(x0, min(x1, w), 2):
            o = (y * w + x) * 3
            tot += 1
            if px[o] + px[o+1] + px[o+2] > 150:
                lit += 1
    return lit / float(tot or 1)


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("net_shot: another qemu-system is running; refusing to start a second one")
        return 2

    off = run(False)
    on  = run(True)

    # The menu drops below the glyph; an address list is visibly more ink than
    # a single "Not connected" line.
    a_off = ink_area(off, 700, 24, 1000, 140)
    a_on  = ink_area(on,  700, 24, 1000, 140)
    print("net_shot: menu ink without a NIC %.1f%%, with one %.1f%%"
          % (a_off * 100, a_on * 100))

    fails = []
    if a_off < 0.005:
        fails.append("the indicator's menu did not open at all without a NIC")
    if a_on <= a_off:
        fails.append("the bar says the same thing with and without a network -- "
                     "it is not reading the link")

    for f in fails:
        print("net_shot: FAIL %s" % f)
    print("net_shot: %s" % ("OK" if not fails else "FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
