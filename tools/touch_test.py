#!/usr/bin/env python3
"""touch_test.py -- put fingers on the glass and see whether the OS felt them.

    make test-touch

A touchscreen is not a mouse with a different name. It reports CONTACTS, each
with a slot number, a position, and a lifetime that ends when its tracking id
goes to -1 -- there is no release event. Between those, the device says
"ABS_MT_SLOT 1" once and then talks about contact 1 until it says otherwise,
which means the driver has to carry state across events that a per-event
switch statement naturally does not.

A driver that gets that wrong does not fail loudly. It reports one finger at
the average of all of them, or leaves a contact on the screen forever after
it has lifted. Both look like working code from inside the guest, so the test
is here rather than there: THIS SIDE puts the fingers down, so this side knows
what the guest's report should say.

    nothing  ->  two fingers down  ->  two contacts  ->  lift one  ->  one left

The middle reading catches averaging; the last catches the missing release.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
SOCK = os.path.join(BUILD, "touch-ser.sock")
QMP = os.path.join(BUILD, "touch-qmp.sock")


class Qmp:
    def __init__(self, path):
        for _ in range(60):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(1)
        else:
            raise SystemExit("touch: QMP never came up")
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, _command, **args):
        msg = {"execute": _command}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("touch: QMP closed")
            r = json.loads(line)
            if "event" in r:
                continue
            if "error" in r:
                raise SystemExit("touch: QMP error: %s" % r["error"])
            return r


def mtt(kind, slot, tid, axis, value):
    return {"type": "mtt",
            "data": {"type": kind, "slot": slot, "tracking-id": tid,
                     "axis": axis, "value": value}}


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("touch: another qemu-system is running; refusing to start a second one")
        return 2

    for p in (SOCK, QMP):
        if os.path.exists(p):
            os.remove(p)
    scratch = os.path.join(BUILD, "touch-root.img")
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch], check=True)

    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        "-device", "virtio-multitouch-pci",
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "touch-qemu.log"), "wb")
    p = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)

    fails = []
    try:
        s = None
        for _ in range(90):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(SOCK)
                break
            except OSError:
                time.sleep(1)
        if s is None:
            raise SystemExit("touch: the guest never opened its serial socket")
        s.settimeout(0.4)
        buf = bytearray()

        def drain(seconds):
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass

        def report(tag):
            buf.clear()
            s.sendall(b"test touch\n")
            drain(10)
            text = buf.decode("utf-8", "replace")
            if "test touch: SKIP" in text:
                fails.append("%s: the guest found no multitouch device" % tag)
                return None
            m = re.findall(r"\[touch\] (\d+) contact\(s\) began, (\d+) down now", text)
            if not m:
                fails.append("%s: the guest never answered `test touch`" % tag)
                return None
            began, down = (int(x) for x in m[-1])
            print("touch: %-18s %d began, %d down" % (tag, began, down))
            return began, down

        drain(70)
        before = report("nothing touching")

        q = Qmp(QMP)
        # TWO FINGERS, at different places. One would not distinguish a driver
        # that tracks contacts from one that tracks a single position.
        q.cmd("input-send-event", events=[
            mtt("begin", 0, 1, "x", 4000), mtt("data", 0, 1, "y", 6000),
        ])
        q.cmd("input-send-event", events=[
            mtt("begin", 1, 2, "x", 20000), mtt("data", 1, 2, "y", 9000),
        ])
        time.sleep(2)
        two = report("two fingers down")

        # LIFT ONE.
        #
        # THE TRACKING ID MUST BE -1 AND THE HOST HAS TO SAY SO. QEMU passes
        # whatever tracking id it is given straight through to the guest; the
        # "end" type only decides that a slot-and-id pair is sent instead of
        # an axis. Its own UI layer sets the id to -1 before calling that
        # path, and a test driving QMP directly has to do the same. Sending
        # "end" with the contact's real id tells the guest the finger is still
        # there -- which is what this test did at first, and it read as a
        # driver that ignored the lift.
        q.cmd("input-send-event", events=[
            mtt("end", 1, -1, "x", 20000),
        ])
        time.sleep(2)
        one = report("one lifted")

        if before and before[1] != 0:
            fails.append("the guest reported %d contact(s) before anything "
                         "touched it" % before[1])
        if two:
            if two[0] < 2:
                fails.append("two fingers went down and the guest counted %d "
                             "contact(s) beginning" % two[0])
            if two[1] != 2:
                fails.append("TWO FINGERS ARE DOWN and the guest says %d -- a "
                             "driver that tracks one position sees exactly "
                             "this" % two[1])
        if one and one[1] != 1:
            fails.append("one finger lifted and the guest still has %d down -- "
                         "the tracking-id end event was not acted on" % one[1])
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    if fails:
        for f in fails:
            print("touch: FAIL -- %s" % f)
        return 1
    print("=== test-touch: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
