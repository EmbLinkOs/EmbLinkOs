#!/usr/bin/env python3
"""Boot aarch64 over and over until it HANGS, and photograph the machine when it does.

    python3 tools/arm64_hang_capture.py [runs] [seconds-per-boot]

WHY THIS EXISTS. A boot stops, roughly one time in twenty, inside the deadline
scheduler's load test -- six CPU-bound threads and one periodic thread on four
cores -- and produces no further output. "It stopped happening" is not a
diagnosis and a killed QEMU leaves nothing behind, so this catches one in the
act: on a boot that makes no progress it reads every vCPU's PC over QMP, twice,
200 ms apart, and symbolizes them against build/aarch64/kernel.elf.

Two samples rather than one, because the question is which KIND of hang it is:
identical PCs on both samples is a core stuck (a spin, a deadlock); PCs that
move within the same function is a livelock; PCs that move freely while the
serial console is silent is something else entirely, and knowing which is the
whole difference between looking at the scheduler and looking at the driver.

Writes the capture to build/aarch64/hang-<n>.txt and stops at the first one.
Exit status 0 when a hang was captured, 1 when every boot completed.
"""
import bisect
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARM = os.path.join(ROOT, "build/aarch64")
DONE = b"all self-tests done"
# A CRASH looks like a hang from outside -- no more output -- but it leaves the
# best evidence there is, and waiting out the full timeout for it wastes a
# minute per run of a soak that already needs twenty runs to catch one.
#
# NOT "=== aarch64 exception ===": a healthy boot prints that FOUR times,
# because the A1 self-tests take deliberate faults and recover from them. The
# marker of a fatal one is the kernel saying it has no way back.
CRASH = (b"kernel halted", b"PANIC")

_syms = []


def symbolize(addr):
    """Nearest preceding symbol from the kernel ELF, as name+offset."""
    if not _syms:
        try:
            out = subprocess.run(["aarch64-elf-nm", "-n", ARM + "/kernel.elf"],
                                 capture_output=True, text=True).stdout
            for line in out.splitlines():
                f = line.split()
                if len(f) == 3 and f[1] in "tTwW":
                    _syms.append((int(f[0], 16), f[2]))
        except Exception:
            pass
        _syms.sort()
    if not _syms:
        return ""
    i = bisect.bisect_right([a for a, _ in _syms], addr) - 1
    if i < 0:
        return ""
    return "  %s+0x%x" % (_syms[i][1], addr - _syms[i][0])


def qmp(port, cmd, args=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    f = s.makefile("rw")
    f.readline()
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    f.write(json.dumps(msg) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            s.close()
            return r.get("return", "")


def sym_addr(name):
    """The address of a kernel symbol, or None."""
    if not _syms:
        symbolize(0)                      # fills _syms as a side effect
    out = subprocess.run(["aarch64-elf-nm", ARM + "/kernel.elf"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name:
            return int(f[0], 16)
    return None


def sched_lock_state(port):
    """WHO holds the scheduler lock. All cores spinning says only that somebody
    never unlocked; the lock itself records the call site that took it."""
    addr = sym_addr("g_sched_lock")
    if addr is None:
        return ["  (g_sched_lock not in the symbol table)"]
    # The kernel window is a fixed offset from physical on this port; read the
    # physical alias, which the monitor can address without a page walk.
    phys = addr - 0xffffffffc0000000 + 0x40000000
    txt = str(qmp(port, "human-monitor-command",
                  {"command-line": "xp /6gx 0x%x" % phys}))
    words = []
    for line in txt.splitlines():
        for tok in line.split(":", 1)[-1].split():
            if tok.startswith("0x"):
                words.append(int(tok, 16))
    if len(words) < 6:
        return ["  (could not read g_sched_lock at %#x)" % phys, "  " + txt.strip()]
    # struct spinlock: uint32 locked (+4 pad), then saved_flags, acquires,
    # contended, spins, holder_lr -- so the counters start at word 2, not 1.
    locked = words[0] & 0xFFFFFFFF
    acquires, contended, spins, holder = words[2], words[3], words[4], words[5]
    return ["  g_sched_lock @ %#x (phys %#x)" % (addr, phys),
            "    locked    : %d" % locked,
            "    acquires  : %d  contended %d  spins %d" % (acquires, contended, spins),
            "    HELD BY   : %#018x%s" % (holder, symbolize(holder) if holder else "  (free)")]


def sample(port):
    """Every vCPU's PC, symbolized."""
    txt = qmp(port, "human-monitor-command", {"command-line": "info registers -a"})
    out = []
    cpu = "?"
    for line in str(txt).splitlines():
        m = re.match(r"CPU#(\d+)", line)
        if m:
            cpu = m.group(1)
        m = re.search(r"\bPC=([0-9a-fA-F]+)", line) or re.search(r"^PC\s*=?\s*([0-9a-fA-F]+)", line)
        if m:
            pc = int(m.group(1), 16)
            out.append("  cpu %s: pc %016x%s" % (cpu, pc, symbolize(pc)))
    return out or ["  (no PC in `info registers -a`)"]


def one_run(n, secs):
    log = "%s/hangrun.log" % ARM
    if os.path.exists(log):
        os.remove(log)
    for name, src in (("rootfs", ARM + "/embkfs-arm64.img"),
                      ("seed", ROOT + "/build/crash-seed.img"),
                      ("swap", ROOT + "/build/swap.img"),
                      ("nvme", ROOT + "/build/nvme-scratch.img")):
        subprocess.run(["cp", src, "%s/hang-%s.img" % (ARM, name)], check=False)
    port = 4800 + (n % 300)
    argv = ["qemu-system-aarch64", "-M", "virt,gic-version=3,accel=hvf", "-cpu", "host",
            "-smp", "4", "-m", "512M",
            "-drive", "file=%s/hang-rootfs.img,format=raw,if=none,id=d0" % ARM,
            "-device", "virtio-blk-pci,drive=d0",
            "-drive", "file=%s/hang-seed.img,format=raw,if=none,id=d1" % ARM,
            "-device", "virtio-blk-pci,drive=d1",
            "-drive", "file=%s/hang-swap.img,format=raw,if=none,id=d2" % ARM,
            "-device", "virtio-blk-pci,drive=d2",
            "-drive", "file=%s/hang-nvme.img,format=raw,if=none,id=nv0" % ARM,
            "-device", "nvme,serial=EMBKSCRATCH,drive=nv0",
            "-device", "virtio-gpu-pci", "-device", "virtio-keyboard-pci",
            "-device", "virtio-tablet-pci",
            "-display", "none", "-serial", "file:" + log,
            "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port,
            "-kernel", ARM + "/kernel.img"]
    p = subprocess.Popen(argv, stderr=subprocess.DEVNULL)
    t0 = time.time()
    done = False
    crashed = False
    while time.time() - t0 < secs:
        try:
            blob = open(log, "rb").read()
            if DONE in blob:
                done = True
                break
            if any(c in blob for c in CRASH):
                crashed = True
                time.sleep(1)          # let the dump finish printing
                break
        except OSError:
            pass
        time.sleep(1)
    cap = None
    if not done:
        first = sample(port)
        time.sleep(0.2)
        second = sample(port)
        try:
            whole = open(log, "rb").read().decode(errors="replace").splitlines()
        except OSError:
            whole = []
        # A crash prints its own dump; keep plenty of context before it. A hang
        # prints nothing, so the tail is all there is.
        tail = whole[-120:] if crashed else whole[-25:]
        held = sched_lock_state(port)
        cap = ["=== aarch64 %s, run %d ===" % ("CRASH" if crashed else
                                               "hang, no progress in %d s" % secs, n),
               "--- serial output ---"] + tail + \
              ["--- the scheduler lock ---"] + held + \
              ["--- sample 1 ---"] + first + ["--- sample 2, 200 ms later ---"] + second
        try:
            open("%s/hang-%d-full.log" % (ARM, n), "w").write("\n".join(whole) + "\n")
        except OSError:
            pass
    p.kill()
    p.wait()
    return done, cap


def main(argv):
    runs = int(argv[1]) if len(argv) > 1 else 20
    secs = int(argv[2]) if len(argv) > 2 else 90
    for n in range(1, runs + 1):
        done, cap = one_run(n, secs)
        print("run %2d: %s" % (n, "completed" if done else "FAILED -- captured"), flush=True)
        if cap:
            path = "%s/hang-%d.txt" % (ARM, n)
            open(path, "w").write("\n".join(cap) + "\n")
            print("\n".join(cap))
            print("\n(written to %s)" % path)
            return 0
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
