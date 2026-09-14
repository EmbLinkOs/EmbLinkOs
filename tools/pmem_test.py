#!/usr/bin/env python3
"""pmem_test.py -- did the write survive the power going away?

    make test-pmem

Persistent memory is the one storage device whose only interesting property
cannot be checked in a single boot. Reading back what you just wrote proves
nothing: the bytes are in memory either way. What has to be proved is that a
write the guest FLUSHED is still there after the machine stopped existing.

So this runs the guest twice over the same backing file and KILLS it in
between. No clean shutdown, deliberately -- a clean shutdown gives the host a
chance to write out a mapping that was never made durable, and a driver that
forgot to flush would pass. The kill is the test.

    boot 1: finds nothing, writes generation 1, flushes
    (killed)
    the FILE on this side holds the signature      <- the host's own reading
    boot 2: finds generation 1, writes generation 2

The middle check is on this side on purpose. "The guest read it back" and
"the bytes reached the file" are different claims, and only the second one is
about persistence.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
BACKING = os.path.join(BUILD, "pmem.img")
SIZE_MB = 64


KIND = sys.argv[1] if len(sys.argv) > 1 else "virtio-pmem"


def run_guest():
    env = dict(os.environ)
    env["MEM"] = "1024M,slots=2,maxmem=4G"
    if KIND == "nvdimm":
        # THE OTHER KIND. No device at all: memory in a slot, and an ACPI
        # table saying it is persistent. Same guest-visible outcome.
        env["EXTRA_QEMU"] = (
            "-machine pc,nvdimm=on "
            "-object memory-backend-file,id=pm0,share=on,mem-path=%s,size=%dM "
            "-device nvdimm,memdev=pm0,id=nv0" % (BACKING, SIZE_MB))
    else:
        env["EXTRA_QEMU"] = (
            "-machine pc,nvdimm=on "
            "-object memory-backend-file,id=pm0,share=on,mem-path=%s,size=%dM "
            "-device virtio-pmem-pci,memdev=pm0,id=nv0" % (BACKING, SIZE_MB))
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "console_test.py"),
                        "test pmem"],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    return p.stdout + p.stderr


def generations(text):
    found = re.search(r"\[pmem\] \S+, \d+ MiB, generation found: (\d+)", text)
    wrote = re.search(r"\[pmem\] generation written: (\d+)", text)
    return (int(found.group(1)) if found else None,
            int(wrote.group(1)) if wrote else None)


def main():
    fails = []

    # A FRESH REGION. Starting from whatever a previous run left would make
    # "generation 1" mean nothing.
    with open(BACKING, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)
    print("pmem: %s, %d MiB backing file, zeroed" % (KIND, SIZE_MB))

    out1 = run_guest()
    f1, w1 = generations(out1)
    if "test pmem: SKIP" in out1:
        print("pmem: the guest found no persistent memory device")
        return 1
    print("pmem: boot 1 -- found %s, wrote %s" % (f1, w1))
    if f1 != 0:
        fails.append("boot 1 found generation %s on a zeroed region" % f1)
    if w1 != 1:
        fails.append("boot 1 wrote generation %s, expected 1" % w1)

    # THE HOST'S OWN READING. The guest is gone; if the signature is in this
    # file then the flush reached the backing store, and if it is not then
    # nothing the guest said about it was true.
    with open(BACKING, "rb") as f:
        head = f.read(16)
    if head[:12] != b"EMBLINK-PMEM":
        fails.append("the backing file does NOT hold the signature after the "
                     "guest was killed -- the flush never reached it (%r)"
                     % head[:12])
    else:
        gen = int.from_bytes(head[12:16], "little")
        print("pmem: the backing file holds generation %d, read from this side" % gen)
        if gen != 1:
            fails.append("the file holds generation %d, expected 1" % gen)

    out2 = run_guest()
    f2, w2 = generations(out2)
    print("pmem: boot 2 -- found %s, wrote %s" % (f2, w2))
    if f2 != 1:
        fails.append("boot 2 found generation %s; the write did not survive" % f2)
    if w2 != 2:
        fails.append("boot 2 wrote generation %s, expected 2" % w2)

    if fails:
        for m in fails:
            print("pmem: FAIL -- %s" % m)
        return 1
    print("=== test-pmem: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
