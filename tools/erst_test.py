#!/usr/bin/env python3
"""erst_test.py -- did the crash record survive the crash?

    make test-erst

docs/PILLARS.md phase 3 wants persistent crash reports. Writing them to the
disk is the obvious way and the wrong one: the disk driver may be what
panicked. ERST is the firmware's answer -- somewhere durable, reached without
touching the operating system's own storage stack.

Which makes the only interesting claim the same one persistent memory makes,
and it cannot be checked in a single boot: reading back what you just wrote
proves nothing. So the guest runs twice over one backing file and is KILLED in
between, and this side reads the file afterwards.

    boot 1: finds nothing, writes generation 1
    (killed)
    the FILE on this side contains the record
    boot 2: finds generation 1, writes generation 2
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
BACKING = os.path.join(BUILD, "erst.img")
SIZE = 0x10000


def run_guest():
    env = dict(os.environ)
    env["MACHINE"] = "q35"
    env["EXTRA_QEMU"] = (
        "-object memory-backend-file,id=erstmem,mem-path=%s,size=0x%x,share=on "
        "-device acpi-erst,memdev=erstmem" % (BACKING, SIZE))
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "console_test.py"),
                        "test erst"],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    return p.stdout + p.stderr


def generations(text):
    found = re.search(r"\[erst\] generation found: (\d+)", text)
    wrote = re.search(r"\[erst\] generation written: (\d+)", text)
    return (int(found.group(1)) if found else None,
            int(wrote.group(1)) if wrote else None)


def main():
    fails = []
    with open(BACKING, "wb") as f:
        f.truncate(SIZE)
    print("erst: %d KiB backing file, zeroed" % (SIZE // 1024))

    out1 = run_guest()
    if "test erst: SKIP" in out1:
        print("erst: the guest found no error record storage")
        return 1
    f1, w1 = generations(out1)
    print("erst: boot 1 -- found %s, wrote %s" % (f1, w1))
    if f1 != 0:
        fails.append("boot 1 found generation %s in a zeroed store" % f1)
    if w1 != 1:
        fails.append("boot 1 wrote generation %s, expected 1" % w1)

    with open(BACKING, "rb") as f:
        blob = f.read()
    if b"EMBLINK-ERST" not in blob:
        fails.append("the backing file does NOT contain the record after the "
                     "guest was killed")
    else:
        print("erst: the record is in the backing file, read from this side")

    out2 = run_guest()
    f2, w2 = generations(out2)
    print("erst: boot 2 -- found %s, wrote %s" % (f2, w2))
    if f2 != 1:
        fails.append("boot 2 found generation %s; the record did not survive" % f2)

    if fails:
        for m in fails:
            print("erst: FAIL -- %s" % m)
        return 1
    print("=== test-erst: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
