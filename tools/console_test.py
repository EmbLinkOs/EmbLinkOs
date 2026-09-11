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
import os, sys, time, signal, subprocess, threading, socket, json, random, bisect, re

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
    # A QMP socket, so a HANG leaves evidence: on any timeout below, every
    # vCPU's RIP and RFLAGS are read through the monitor and symbolized against
    # the kernel. "It never came back" says nothing; "cpu 2 is in
    # spin_lock+0x1c called from vma_munmap, IF=0" says almost everything.
    qmp_port = 4900 + random.randint(0, 400)
    argv += ["-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port]
    timeout = float(os.environ.get("TIMEOUT", "400"))

    def hmp(cmd):
        try:
            sk = socket.create_connection(("127.0.0.1", qmp_port), timeout=3.0)
            f = sk.makefile("rw")
            f.readline()                                   # the greeting
            f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
            f.write(json.dumps({"execute": "human-monitor-command",
                                "arguments": {"command-line": cmd}}) + "\n"); f.flush()
            resp = json.loads(f.readline()); sk.close()
            return resp.get("return", "")
        except Exception as e:
            return "(qmp failed: %s)" % e

    _syms = []
    def symbolize(addr):
        if not _syms:
            try:
                out = subprocess.run(["x86_64-elf-nm", "-n", os.path.join(ROOT, "kernel", "kernel.elf")],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    parts = line.split()
                    if len(parts) == 3 and parts[1] in "tTwW":
                        _syms.append((int(parts[0], 16), parts[2]))
            except Exception:
                pass
            if not _syms: _syms.append((0, "?"))
        i = bisect.bisect_right([a for a, _ in _syms], addr) - 1
        if i < 0: return "?"
        return "%s+0x%x" % (_syms[i][1], addr - _syms[i][0])

    def hang_report(why):
        print("\nconsole_test: %s -- where each core is (5 samples, 200 ms apart):" % why, file=sys.stderr)
        for sample in range(5):
          if sample: time.sleep(0.2)
          regs = hmp("info registers -a")
          for block in regs.split("CPU#")[1:]:
            cpu = block.split()[0]
            rip = re.search(r"RIP=([0-9a-f]+)", block); rfl = re.search(r"RFL=([0-9a-f]+)", block)
            rsp = re.search(r"RSP=([0-9a-f]+)", block); cs = re.search(r"CS =([0-9a-f]+)", block)
            if not rip: continue
            a = int(rip.group(1), 16); flags = int(rfl.group(1), 16) if rfl else 0
            print("  [%d] cpu %s: rip %016x  %s  rsp %s  cs %s  IF=%d" % (
                sample, cpu, a, symbolize(a) if a >= 0xffffffff80000000 else "(user)",
                rsp.group(1) if rsp else "?", cs.group(1) if cs else "?", 1 if flags & 0x200 else 0),
                file=sys.stderr)

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

    # KEYSTROKES A TEST ASKS FOR. A few tests prove the real keyboard IRQ path
    # and so cannot fake the key from software: they print a marker and wait.
    # Seen while waiting for a verdict, the key is sent through the monitor --
    # once per marker occurrence.
    KEY_ON_MARKER = {b"(send ^C now)": "sendkey ctrl-c"}
    sent = {}
    def wait_for(needle, secs):
        dl = time.time() + secs; nb = needle.encode()
        while time.time() < dl:
            with lock:
                if nb in buf: return True
                pending = [(m, k) for m, k in KEY_ON_MARKER.items() if buf.count(m) > sent.get(m, 0)]
            for m, k in pending:
                time.sleep(0.3)
                hmp(k)
                sent[m] = sent.get(m, 0) + 1
            time.sleep(0.05)
        return False

    failures = 0
    try:
        if not wait_for("first frame presented", 240):
            hang_report("the desktop never came up"); return 1
        time.sleep(1.5)
        for c in cmds:
            with lock: buf.clear()
            sent.clear()
            p.stdin.write((c + "\n").encode()); p.stdin.flush()
            if not wait_for("[cmd] " + c, timeout):
                hang_report("NO VERDICT for %r within %ds -- stopping" % (c, timeout))
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
