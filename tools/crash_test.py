#!/usr/bin/env python3
"""crash_test.py -- crash the kernel, take the power away, ask it what happened.

    make test-crash

docs/PILLARS.md phase 3: "A problem on the real machine cannot be diagnosed
after the fact." Until now a kernel fault printed a register dump to a serial
port and halted. On a developer's desk that is enough. On the target machine
there is no cable, the screen holds whatever was on it, and the next thing
that happens is somebody pressing the power button.

So the only interesting claim is one that cannot be checked in a single boot,
and the test has the same shape as the persistent-memory one:

    boot 1: no record; crash it ON PURPOSE
    (killed -- no clean shutdown, because a crash is not one)
    the FILE on this side holds the record      <- the host's own reading
    boot 2: the machine says what happened last time

THREE THINGS ARE CHECKED IN BOOT 2, and the third is the one that would catch
a report that is merely plausible:

  - the fault VECTOR is 13, which is the #GP `test panicnow` deliberately
    causes and not the page fault that most bugs produce;
  - the KERNEL LOG carried across contains the line panicnow printed just
    before faulting -- the part that says what it was DOING, which a register
    dump never does;
  - the recorded RIP resolves, through this host's `nm` on the very kernel
    that crashed, to a real function. A stored constant, a truncated field or
    a byte-swapped one all survive the first two checks and fail this.
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
STORE = os.path.join(BUILD, "crash-erst.img")
SOCK = os.path.join(BUILD, "crash-ser.sock")
STORE_SIZE = 0x10000


def boot(tag):
    """Start the guest and return (process, connected socket)."""
    if os.path.exists(SOCK):
        os.remove(SOCK)
    scratch = os.path.join(BUILD, "crash-root.img")
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch], check=True)
    argv = [
        "qemu-system-x86_64", "-cpu", "max", "-machine", "q35",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        "-object", "memory-backend-file,id=erstmem,mem-path=%s,size=0x%x,share=on"
                   % (STORE, STORE_SIZE),
        "-device", "acpi-erst,memdev=erstmem",
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        # WITHOUT `nowait`, DELIBERATELY. With it, QEMU starts the guest
        # immediately and everything printed before this side manages to
        # connect is thrown away -- which for boot 2 is exactly the crash
        # banner, printed within the first second. It made this test pass or
        # fail depending on how busy the machine was. Blocking until the
        # harness is listening makes the first byte of the boot observable.
        "-serial", "unix:%s,server" % SOCK,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "crash-qemu-%s.log" % tag), "wb")
    p = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
    s = None
    for _ in range(90):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(SOCK)
            break
        except OSError:
            time.sleep(1)
    if s is None:
        p.kill()
        raise SystemExit("crash: the guest never opened its serial socket")
    s.settimeout(0.4)
    return p, s


def drain(s, buf, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            b = s.recv(65536)
            if not b:
                break
            buf.extend(b)
        except Exception:
            pass


def wait_for(s, buf, needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            b = s.recv(65536)
            if b:
                buf.extend(b)
        except Exception:
            pass
        if needle.encode() in bytes(buf):
            return True
    return False


def symbol_for(rip):
    """Resolve a kernel address with this host's nm, against the kernel that
    crashed.

    DELIBERATELY NOT THE GUEST'S OWN ANSWER. The guest symbolises the address
    itself now (kernel/lib/ksym.c, against /system/kernel.embdbg) and prints
    "func+0xN (file:line)" in its crash banner -- but a test that checked that
    string would be asking the record and the symbolizer to agree with each
    other, which they would even if both were reading a bad address. Resolving
    it here, independently, against the very kernel that crashed, is what makes
    the ADDRESS the thing under test.

    CONTAINMENT, NOT PROXIMITY. The first version of this capped the distance
    from the preceding symbol at 16 KiB and rejected a perfectly good address:
    `test panicnow` faults inside selftests_handle_command, which at -O0 is a
    43 KiB function. A distance is a guess about how big functions are. The
    real question is whether the address lies between a TEXT symbol and the
    next symbol of any kind, which is what a symbolizer actually asks."""
    elf = os.path.join(ROOT, "kernel", "kernel.elf")
    try:
        out = subprocess.run(["x86_64-elf-nm", "-n", elf],
                             capture_output=True, text=True, check=True).stdout
    except Exception:
        return None
    prev = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if addr > rip:
            # `prev` is the symbol this address belongs to, and `addr` is where
            # that symbol's span ends.
            break
        prev = (addr, parts[1], parts[2])
    if not prev:
        return None
    if prev[1] not in ("t", "T"):
        return None                      # in data, not code: not a real RIP
    return "%s+0x%x" % (prev[2], rip - prev[0])


def main():
    fails = []

    with open(STORE, "wb") as f:
        f.truncate(STORE_SIZE)
    print("crash: %d KiB error store, zeroed" % (STORE_SIZE // 1024))

    # ---- boot 1: nothing stored, then break it on purpose ----------------
    p, s = boot("1")
    try:
        buf = bytearray()
        drain(s, buf, 70)
        buf.clear()
        s.sendall(b"test crashlog\n")
        drain(s, buf, 8)
        first = buf.decode("utf-8", "replace")
        if "no crash record from a previous boot" not in first:
            fails.append("boot 1 reported a crash record in a zeroed store")
        else:
            print("crash: boot 1 -- no record, as expected")

        buf.clear()
        s.sendall(b"test panicnow\n")
        if not wait_for(s, buf, "kernel-mode fault -- system halted.", 60):
            fails.append("the kernel did not fault when told to")
        else:
            print("crash: boot 1 -- faulted on purpose and halted")
        # Give the record time to reach the store before the power goes off.
        drain(s, buf, 3)
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    # ---- the host's own reading ------------------------------------------
    with open(STORE, "rb") as f:
        blob = f.read()
    if b"EMBLINK-CRASH" not in blob:
        fails.append("the error store does NOT contain the crash record after "
                     "the machine was killed")
        for m in fails:
            print("crash: FAIL -- %s" % m)
        return 1
    print("crash: the record is in the store, read from this side")

    # ---- boot 2: what does it say happened? ------------------------------
    p, s = boot("2")
    try:
        buf = bytearray()
        drain(s, buf, 70)
        text = buf.decode("utf-8", "replace")
        buf.clear()
        s.sendall(b"test crashlog\n")
        drain(s, buf, 10)
        full = buf.decode("utf-8", "replace")
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    if "THE LAST BOOT ENDED IN A KERNEL FAULT" not in text:
        fails.append("boot 2 did not report the previous crash at all")
        for m in fails:
            print("crash: FAIL -- %s" % m)
        return 1

    m = re.search(r"crash: vector (\d+), error code ([0-9a-f]+)", text)
    if not m:
        fails.append("the report has no vector line")
    else:
        vector = int(m.group(1))
        print("crash: boot 2 -- reported vector %d" % vector)
        if vector != 13:
            fails.append("the fault was a #GP (vector 13) and the record says "
                         "vector %d" % vector)

    m = re.search(r"crash: RIP ([0-9a-f]+)", text)
    if not m:
        fails.append("the report has no RIP line")
    else:
        rip = int(m.group(1), 16)
        sym = symbol_for(rip)
        if sym is None:
            fails.append("the recorded RIP %#x does not fall inside any "
                         "FUNCTION of the kernel that crashed -- a stored "
                         "constant, a truncated field or a byte-swapped one "
                         "all look like this" % rip)
        else:
            print("crash: the recorded RIP is %s, resolved on this side" % sym)

    if "[panicnow] faulting the kernel ON PURPOSE" not in text:
        fails.append("the kernel log did not survive -- the register dump came "
                     "across and the thing that says WHAT IT WAS DOING did not")
    else:
        print("crash: the kernel log leading up to the fault came across")

    if "[crashlog]" not in full:
        fails.append("`test crashlog` did not print the full record")
    elif len(full) <= 200:
        fails.append("`test crashlog` printed almost nothing")
    else:
        print("crash: the full log is available on request (%d bytes printed)"
              % len(full))

    if fails:
        for m in fails:
            print("crash: FAIL -- %s" % m)
        return 1
    print("=== test-crash: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
