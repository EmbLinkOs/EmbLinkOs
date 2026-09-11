#!/usr/bin/env python3
"""console_test.py -- boot the x86 kernel headless, drive its serial console,
and judge each command by the kernel's own verdict line.

The x86 self-tests have always been run by typing at the kernel console. This
is that, scripted: QEMU with -serial stdio, wait for the desktop to come up,
send each command, wait for its `[cmd] <name>: ...` line, and report OK/FAIL
from what the kernel printed. The aarch64 side has had a scripted boot test
for a long time (arch.mk, test-arm64-boot); x86 had audio_test.py for one
device and nothing general.

    python3 tools/console_test.py "test mmap" "test aslr"
    EXTRA_DISK=build/crash-scratch.img python3 tools/console_test.py "test embkfs crash"

Options via the environment, because a Makefile target sets them and a person
rarely needs to:
    EXTRA_DISK   a third raw image, attached as IDE index 2 (the kernel names
                 it sdc). Used by tests that want a disk they may destroy.
    SWAP_DISK    a raw image with an EMBKSWAP header (tools/mkswap.py),
                 attached as IDE index 3; the kernel finds it by the header.
    SMP, MEM     cores (4) and memory (2G).
    TIMEOUT      seconds to wait for each command's verdict (400).

Exit status is the number of commands whose verdict line did not say OK, so
`make` can fail on it. A command that never produced a verdict counts as
failed and the run stops there -- a hung kernel is a failure, not a timeout
to shrug at.

ONE QEMU AT A TIME. Two guests on one host starve each other and produce
scheduler-timing failures that are not in the kernel; this file refuses to
start if another qemu-system is running rather than manufacture one.
"""
import os, sys, time, signal, subprocess, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main(cmds):
    if not cmds:
        print(__doc__); return 2
    if subprocess.run(["pgrep", "-f", "qemu-system"], capture_output=True).returncode == 0:
        print("console_test: another qemu-system is running; refusing to start a second one", file=sys.stderr)
        return 2

    extra = os.environ.get("EXTRA_DISK")
    argv = ["qemu-system-x86_64", "-cpu", "max",
            "-drive", "format=raw,file=%s/myos.img,if=ide,index=0" % ROOT,
            "-drive", "format=raw,file=%s/embkfs.img,if=ide,index=1" % ROOT]
    if extra:
        argv += ["-drive", "format=raw,file=%s,if=ide,index=2" % os.path.abspath(extra)]
    swap = os.environ.get("SWAP_DISK")
    if swap:
        argv += ["-drive", "format=raw,file=%s,if=ide,index=3" % os.path.abspath(swap)]
    argv += ["-serial", "stdio", "-no-reboot", "-no-shutdown",
             "-m", os.environ.get("MEM", "2G"), "-smp", os.environ.get("SMP", "4"),
             "-display", "none"]
    timeout = float(os.environ.get("TIMEOUT", "400"))

    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
    buf = bytearray(); lock = threading.Lock()
    def reader():
        while True:
            b = p.stdout.read(1)
            if not b: break
            sys.stdout.buffer.write(b); sys.stdout.flush()
            with lock: buf.extend(b)
    threading.Thread(target=reader, daemon=True).start()

    def wait_for(needle, secs):
        dl = time.time() + secs; nb = needle.encode()
        while time.time() < dl:
            with lock:
                if nb in buf: return True
            time.sleep(0.05)
        return False

    failures = 0
    try:
        if not wait_for("first frame presented", 240):
            print("\nconsole_test: the desktop never came up", file=sys.stderr); return 1
        time.sleep(1.5)
        for c in cmds:
            with lock: buf.clear()
            p.stdin.write((c + "\n").encode()); p.stdin.flush()
            if not wait_for("[cmd] " + c, timeout):
                print("\nconsole_test: NO VERDICT for %r within %ds -- stopping" % (c, timeout), file=sys.stderr)
                failures += 1 + (len(cmds) - cmds.index(c) - 1)
                break
            with lock: text = bytes(buf).decode("utf-8", "replace")
            line = [l for l in text.splitlines() if l.startswith("[cmd] " + c)][-1]
            ok = (": OK" in line) or line.rstrip().endswith("OK") or "-> OK" in line
            print("\nconsole_test: %s -> %s" % (c, "OK" if ok else "FAIL"), file=sys.stderr)
            if not ok: failures += 1
            time.sleep(0.3)
    finally:
        p.send_signal(signal.SIGTERM)
        try: p.wait(5)
        except Exception: p.kill()
    return failures

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
