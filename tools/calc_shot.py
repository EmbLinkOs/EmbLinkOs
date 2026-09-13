#!/usr/bin/env python3
"""calc_shot.py -- does the calculator calculate?

    python3 tools/calc_shot.py

A screenshot can show that a calculator's buttons light up. It cannot show that
the machine added anything, because reading "15" off a bitmap is a harder
problem than the one being tested -- so the app says its answer on the log when
= is pressed, and this drives the real keys with a real pointer and reads that
line back.

FOUR SUMS, chosen so a plausible wrong implementation fails at least one:

    7 + 8 =        15    the simplest thing that can work
    2 + 3 x 4 =    20    IMMEDIATE EXECUTION, not precedence. An implementation
                         that quietly buffered would answer 14, and both answers
                         look reasonable until you decide which calculator this
                         is. Keys with no visible expression cannot hold
                         anything back, so 20 is the correct answer here.
    9 / 0 =        Error not a silent no-op and not infinity. The one thing a
                         calculator must never do is look like it accepted
                         input it discarded.
    5 - 8 =        -3    a negative result survives the integer formatting

Each sum starts with C, so one failing does not cascade into the next and
report four failures for one bug.
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

# Key centres, measured off a screenshot of this window at 1024x768. app_shot
# centres the window, so they are stable between runs -- and when they are not,
# the expected line simply never appears and the failure says which sum.
KEY = {
    "C": (406, 308), "/": (621, 308),
    "7": (407, 368), "8": (477, 368), "9": (547, 368), "x": (617, 368),
    "4": (407, 428), "5": (478, 428), "6": (549, 428), "-": (618, 428),
    "1": (406, 488), "2": (475, 488), "3": (544, 488), "+": (615, 488),
    "0": (406, 548), ".": (473, 548), "<": (541, 548), "=": (615, 548),
}

CASES = [
    (["7", "+", "8", "="], "15"),
    (["2", "+", "3", "x", "4", "="], "20"),
    (["9", "/", "0", "="], "Error"),
    (["5", "-", "8", "="], "-3"),
]


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("calc_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("calc", os.path.join(A.BUILD, "shot-calcrun.img"))
    fails = []
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        buf = bytearray()

        def drain(seconds):
            s.settimeout(0.3)
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass

        drain(22)
        s.sendall(b"run /data/apps/calc/calc.elf\n")
        drain(12)

        for keys, want in CASES:
            buf.clear()
            for k in ["C"] + keys:
                S.move(q, *KEY[k])
                S.click(q)
                time.sleep(0.25)
            drain(2)
            text = buf.decode("utf-8", "replace")
            got = re.findall(r"^calc: = (.+)$", text, re.M)
            answer = got[-1].strip() if got else None
            print("calc_shot: %-18s -> %s (wanted %s)"
                  % (" ".join(keys), answer, want))
            if answer != want:
                fails.append("%s gave %s, wanted %s" % (" ".join(keys), answer, want))

        open(os.path.join(A.BUILD, "calc-serial.log"), "w").write(
            buf.decode("utf-8", "replace"))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: " + f)
    print("=== calc-shot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
