#!/usr/bin/env python3
"""tools/lock_test.py -- the screen lock, from the menu bar to the password.

WHAT IS ACTUALLY BEING TESTED. Not a dialog: a division of authority. The
desktop cannot check a password -- /etc/shadow is outside every session's
namespace on purpose -- so it asks authd, which init starts outside the
session and tells whose session it is. A lock that "works" because the desktop
compared two strings itself would look identical from the outside and would be
worth nothing, so every step here is checked at the point where the two
processes meet: what the desktop says, and what authd says.

Four claims, in the order a person meets them:

  1. A session whose user has NO account cannot be locked. The development
     auto-login opens exactly such a session, and a lock screen there would be
     a door with no key. The refusal is the correct behaviour and it is the
     first thing checked.
  2. A real account can lock: menu bar -> EmbLink -> Lock Screen, and the
     screen goes from a desktop to a lock -- measured as ink in the dock's
     band, which the lock covers.
  3. A WRONG password is refused, by authd, and the screen stays locked.
  4. The right one comes back, and the desktop is there again.

The account comes from first-boot setup, typed on the real keyboard, the way
tools/login_test.py does it -- this image ships no accounts, which is why
claim 1 is possible at all.
"""
import json
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import shell_shot as S

USER = "tester"
PASSWORD = "correct horse 1"
WRONG = "hunter2 wrong"

MENU_EMBLINK = (75, 12)         # the menu bar's system menu
MENU_LOCK    = (150, 81)        # "Lock Screen" in it -- measured, build/lock-menu.ppm
DOCK_BAND    = (400, 690, 630, 745)   # x0, y0, x1, y1: the dock pill
# The desktop icon in the TOP-LEFT, inside the rectangle the menu bar's window
# grows to while a dropdown is open (1016x340) -- which is exactly the state
# the machine is in when Lock Screen is chosen from that menu. The first
# version of the lock was drawn translucent and that whole rectangle stayed
# showing the desktop, so this band is checked as well as the dock's.
ICON_BAND    = (20, 45, 95, 120)

KEYS = {' ': 'spc', '\t': 'tab', '\n': 'ret', '-': 'minus', '_': 'shift-minus', '.': 'dot'}


def keyname(c):
    if c in KEYS: return KEYS[c]
    if c.isdigit() or ('a' <= c <= 'z'): return c
    if 'A' <= c <= 'Z': return 'shift-' + c.lower()
    raise ValueError("no key for %r" % c)


def ink(path, band, thresh=110):
    """Lit pixels in a band -- how much is DRAWN there. The dock is bright
    chips on a dark desktop; the lock's wash is near-black and covers it."""
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
        print("lock_test: another qemu-system is running; refusing to start a second one",
              file=sys.stderr)
        return 2

    scratch = tempfile.mkdtemp(prefix="lock_test.")
    disk = os.path.join(scratch, "embkfs.img")
    shutil.copyfile(os.path.join(ROOT, "embkfs.img"), disk)
    qmp_port = 4900 + random.randint(0, 400)
    argv = ["qemu-system-x86_64", "-cpu", "max",
            "-drive", "format=raw,file=%s/myos.img,if=ide,index=0" % ROOT,
            "-drive", "format=raw,file=%s,if=ide,index=1" % disk,
            "-vga", "none", "-device", "virtio-vga,xres=1024,yres=768",
            "-serial", "stdio", "-no-reboot", "-no-shutdown", "-m", "2G", "-smp", "4",
            "-display", "none", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port]
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
    buf = bytearray()
    lock = threading.Lock()

    def reader():
        while True:
            b = p.stdout.read(1)
            if not b:
                break
            sys.stdout.buffer.write(b)
            sys.stdout.flush()
            with lock:
                buf.extend(b)
    threading.Thread(target=reader, daemon=True).start()

    class Qmp:
        def __init__(self, port):
            for _ in range(60):
                try:
                    self.s = socket.create_connection(("127.0.0.1", port), timeout=3.0)
                    break
                except OSError:
                    time.sleep(0.25)
            self.f = self.s.makefile("rw")
            self.f.readline()
            self.cmd("qmp_capabilities")

        def cmd(self, _command, **args):
            msg = {"execute": _command}
            if args:
                msg["arguments"] = args
            self.f.write(json.dumps(msg) + "\n")
            self.f.flush()
            while True:
                r = json.loads(self.f.readline())
                if "event" not in r:
                    return r

        def screendump(self, path):
            if os.path.exists(path):
                os.remove(path)
            self.cmd("screendump", filename=path)
            for _ in range(50):
                if os.path.exists(path) and os.path.getsize(path) > 1024:
                    return
                time.sleep(0.2)

    q = None
    mark = [0]

    def wait_for(needle, secs):
        dl = time.time() + secs
        nb = needle.encode()
        while time.time() < dl:
            with lock:
                i = buf.find(nb, mark[0])
                if i >= 0:
                    mark[0] = i + len(nb)
                    return True
            time.sleep(0.05)
        return False

    def saw(needle, since):
        with lock:
            return needle.encode() in bytes(buf[since:])

    def console(cmd):
        p.stdin.write((cmd + "\n").encode())
        p.stdin.flush()

    def type_text(s):
        for c in s:
            q.cmd("human-monitor-command", **{"command-line": "sendkey " + keyname(c)})
            time.sleep(0.12)

    def click_lock_menu():
        S.move(q, *MENU_EMBLINK); S.click(q)
        time.sleep(2)
        S.move(q, *MENU_LOCK); S.click(q)
        time.sleep(2)

    fails = 0

    def step(ok, what):
        nonlocal fails
        print("\nlock_test: [%s] %s" % ("ok" if ok else "FAIL", what), file=sys.stderr)
        if not ok:
            fails += 1
        return ok

    try:
        if not step(wait_for("TopBar: first frame presented", 300),
                    "the auto-login desktop came up"):
            return 1
        # From the START of the log: authd is up before the desktop is, so by
        # the time the desktop's first frame is announced this line is already
        # behind the cursor wait_for() reads from.
        step(saw("authd: serving /run/emlink.auth for 'yves' (no account", 0),
             "init started a password checker for the session, and it says there is "
             "no account to check against")
        q = Qmp(qmp_port)
        time.sleep(2)

        # 1. THE REFUSAL. A session with no account must not be lockable.
        since = len(buf)
        click_lock_menu()
        time.sleep(2)
        step(saw("home: lock refused -- this session's user has no account", since),
             "locking a session whose user has no account is refused, not shown")

        # An account, typed into first-boot setup, then used at the greeter.
        start = len(buf)
        console("sessions")
        time.sleep(1.5)
        with lock:
            m = re.search(r"session (\d+)\s+user yves", bytes(buf[start:]).decode("utf-8", "replace"))
        if not step(m is not None, "the kernel lists yves's session"):
            return fails
        console("session end %s" % m.group(1))
        if not step(wait_for("first-boot setup started", 60) and
                    wait_for("EmbLink OS Setup: first frame presented", 120),
                    "logging out reaches first-boot setup"):
            return fails
        time.sleep(2.0)
        type_text("%s\t%s\t%s\n" % (USER, PASSWORD, PASSWORD))
        if not step(wait_for("setup: account '%s' created" % USER, 180),
                    "an account was created"):
            return fails
        if not step(wait_for("EmbLink OS Login: first frame presented", 120),
                    "the greeter appeared"):
            return fails
        time.sleep(2.0)
        type_text("%s\t%s\n" % (USER, PASSWORD))
        if not step(wait_for("init: authenticated session '%s'" % USER, 180),
                    "the account opened a session"):
            return fails
        step(wait_for("authd: serving /run/emlink.auth for '%s'" % USER, 60),
             "the new session got its own password checker, named for its user")
        if not step(wait_for("TopBar: first frame presented", 180),
                    "%s's desktop came up" % USER):
            return fails
        time.sleep(3)
        S._pos[0] = None                      # the pointer was re-homed by the new session

        during = os.path.join(ROOT, "build", "lock-during.ppm")
        after = os.path.join(ROOT, "build", "lock-after.ppm")

        # 2. LOCK.
        since = len(buf)
        click_lock_menu()
        time.sleep(2)
        locked = saw("home: screen LOCKED", since)
        step(locked, "the menu bar's Lock Screen locked the screen")
        q.screendump(during)
        b = ink(during, DOCK_BAND)
        i = ink(during, ICON_BAND)
        print("lock_test: while locked, dock band %d lit pixels, desktop icon band %d"
              % (b, i), file=sys.stderr)
        step(b == 0, "the lock covers the dock completely (%d lit pixels left)" % b)
        step(i == 0, "...and the desktop icons under the menu bar's window (%d left)" % i)

        # 2b. THE KERNEL'S OWN SHORTCUTS. GUI+Tab cycles windows and is
        # performed by the kernel, so nothing on screen can decline it: over a
        # locked screen it would raise an application window -- the session,
        # handed to whoever pressed the key.
        since = len(buf)
        q.cmd("human-monitor-command", **{"command-line": "sendkey meta_l-tab"})
        time.sleep(2)
        # The kernel says it swallowed one. Without this line the check below
        # would pass just as well if the keystroke had never arrived at all.
        step(saw("compositor: system shortcut", since),
             "the keystroke reached the kernel and the kernel refused it")
        switched = os.path.join(ROOT, "build", "lock-switch.ppm")
        q.screendump(switched)
        sw_ink = ink(switched, DOCK_BAND)
        print("lock_test: after GUI+Tab, dock band %d lit pixels" % sw_ink, file=sys.stderr)
        step(sw_ink == 0, "GUI+Tab raises nothing over the lock (%d lit pixels)" % sw_ink)

        # 3. A WRONG PASSWORD.
        since = len(buf)
        type_text(WRONG + "\n")
        step(wait_for("authd: refused (1 in a row", 60),
             "a wrong password is refused by authd, not by the desktop")
        time.sleep(2)
        step(not saw("home: screen unlocked", since), "and the screen stays locked")

        # 4. THE RIGHT ONE.
        since = len(buf)
        type_text(PASSWORD + "\n")
        step(wait_for("authd: unlocked", 60), "the right password is accepted by authd")
        step(wait_for("home: screen unlocked", 30), "and the desktop comes back")
        time.sleep(3)
        q.screendump(after)
        c = ink(after, DOCK_BAND)
        print("lock_test: dock band back to %d lit pixels" % c, file=sys.stderr)
        # The dock is dark chips with bright icons on a dark desktop: a few
        # hundred lit pixels is what it looks like, and zero is what the lock
        # looked like. The claim is that it is DRAWN again, not how bright.
        step(c > 300, "the desktop is really there again (%d lit pixels)" % c)
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(5)
        except Exception:
            p.kill()
        shutil.rmtree(scratch, ignore_errors=True)

    print("\n=== test-lock: %s (%d step(s) failed)" % ("FAIL" if fails else "OK", fails),
          file=sys.stderr)
    return fails


if __name__ == "__main__":
    sys.exit(main())
