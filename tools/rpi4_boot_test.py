#!/usr/bin/env python3
"""Boot the Raspberry Pi 4 kernel on QEMU raspi4b and check what it claims.

    tools/rpi4_boot_test.py <kernel.img> <bcm2711-rpi-4-b.dtb> <log> <marker>...

Used by `make ARCH=aarch64 BOARD=rpi4 test-rpi4-boot` -- docs/RPI4.md.
kernel.elf is expected beside kernel.img (for one symbol, below); set NM to
the aarch64 nm if `aarch64-elf-nm` is not it.

Two witnesses, because a Pi on a television has two outputs:

  * the SERIAL log, one grep per marker the current phase claims;
  * the SCREEN, twice:
      1. at fb_init, while rawcon still owns it (fb_init clears it): the kernel log drawn in
         rawcon's foreground colour, and rawcon's blue "newest line" bar. The
         bar is the byte-order check -- with red and blue swapped it comes out
         orange, and the serial log cannot tell you that;
      2. at the end of the boot, after the real console took the screen over:
         the log is STILL there, i.e. nothing between the two left the TV dark.

And one measurement, because a log line cannot make it: EVERY CORE'S OWN
TIMER TICKS (P2). The kernel's per-core interrupt counters (percpu_irqs, in
irq/gic.c) are read twice, 5 s apart, through QMP's `xp` on their physical
address, and each must have grown. The kernel's own "every core is taking
interrupts" self-test cannot be the witness on a Pi with no disk yet: it
samples ~50 ms of guest time after bring-up, and an idle core sleeps up to
SCHED_IDLE_CAP_MS (1 s) between ticks.

WHY A BREAKPOINT for screenshot 1: the kernel gets from rawcon_init to
fb_init in milliseconds, so no amount of watching the serial log can
catch the screen in between. QEMU is started paused with its gdb stub on, and
this script speaks just enough of the gdb remote protocol to put a breakpoint
on fb_init, continue, and wait for the stop. Deterministic, and no gdb
has to be installed.

It waits for the kernel's last word -- the end of the boot self-test, or the
point where an earlier phase stops -- rather than a fixed time, so a slow host
takes longer instead of failing.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

# rawcon.c's palette.
FG = (0xD8, 0xDC, 0xE4)
MARK = (0x5B, 0x8C, 0xFF)

END_OF_BOOT = ("--- all self-tests done", "gic: FATAL", "FATAL no interrupt controller")
TIMEOUT_S = 120
KERNEL_VIRT_BASE = 0xFFFFFFFF80000000
TICK_WINDOW_S = 5
HANDOVER = "fb_init"      # it clears the screen: rawcon's last moment


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def connect(port):
    deadline = time.time() + 10
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT_S)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


class Qmp:
    def __init__(self, port):
        self.f = connect(port).makefile("rw")
        self.f.readline()                              # the greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:                                    # skip async events
            reply = json.loads(self.f.readline())
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError(reply["error"])
            return reply


class Gdb:
    """The four packets of the gdb remote protocol this needs."""

    def __init__(self, port):
        self.s = connect(port)

    def send(self, payload):
        pkt = "$%s#%02x" % (payload, sum(payload.encode()) % 256)
        self.s.sendall(pkt.encode())
        return self.reply()

    def reply(self):
        buf = b""
        while True:
            chunk = self.s.recv(4096)
            if not chunk:
                raise RuntimeError("gdb stub closed the connection")
            buf += chunk
            start = buf.find(b"$")
            end = buf.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and end >= 0 and len(buf) >= end + 3:
                self.s.sendall(b"+")                   # acknowledge it
                return buf[start + 1:end].decode(errors="replace")


def symbol(elf, name):
    nm = os.environ.get("NM", "aarch64-elf-nm")
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit("rpi4_boot_test: no symbol %s in %s" % (name, elf))


def read_ppm(path):
    data = open(path, "rb").read()
    fields, i = [], 0
    while len(fields) < 4:                             # P6 w h maxval
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i:i + w * h * 3]


def count(px, rgb):
    needle = bytes(rgb)
    return sum(1 for k in range(0, len(px), 3) if px[k:k + 3] == needle)


def bright(px):
    """Pixels light enough to be text on any of this kernel's dark consoles."""
    return sum(1 for k in range(0, len(px), 3)
               if px[k] > 0xA0 and px[k + 1] > 0xA0 and px[k + 2] > 0xA0)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    kernel, dtb, log = sys.argv[1:4]
    markers = sys.argv[4:]
    elf = os.path.join(os.path.dirname(kernel), "kernel.elf")
    handover = symbol(elf, HANDOVER)
    irqs_phys = symbol(elf, "percpu_irqs") - KERNEL_VIRT_BASE

    qmp_port, gdb_port = free_port(), free_port()
    if os.path.exists(log):
        os.remove(log)
    qemu = subprocess.Popen(
        ["qemu-system-aarch64", "-M", "raspi4b", "-dtb", dtb,
         "-display", "none", "-monitor", "none",
         "-serial", "file:" + log,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % qmp_port,
         "-gdb", "tcp:127.0.0.1:%d" % gdb_port, "-S",
         "-kernel", kernel],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    tmp = tempfile.mkdtemp()
    shot_rawcon = os.path.join(tmp, "rawcon.ppm")
    shot_end = os.path.join(tmp, "end.ppm")
    reached_handover = False
    try:
        qmp = Qmp(qmp_port)
        gdb = Gdb(gdb_port)
        if gdb.send("Z0,%x,4" % handover) != "OK":
            raise RuntimeError("could not set a breakpoint on %s" % HANDOVER)

        # Continue, and wait for either the breakpoint or the end of the boot:
        # an earlier phase (P0) stops before fb_init is ever reached.
        gdb.s.settimeout(1)
        gdb.s.sendall(b"$c#63")
        deadline = time.time() + TIMEOUT_S
        while time.time() < deadline:
            try:
                stop = gdb.reply()
                reached_handover = stop.startswith(("T05", "S05"))
                break
            except socket.timeout:
                text = open(log, errors="replace").read() if os.path.exists(log) else ""
                if any(m in text for m in END_OF_BOOT):
                    break
        qmp.cmd("stop")
        qmp.cmd("screendump", filename=shot_rawcon)

        if reached_handover:
            gdb.s.settimeout(TIMEOUT_S)
            gdb.send("z0,%x,4" % handover)
            qmp.cmd("cont")
            while time.time() < deadline:
                text = open(log, errors="replace").read()
                if any(m in text for m in END_OF_BOOT):
                    break
                time.sleep(1)
            time.sleep(1)                              # let the last line draw
            qmp.cmd("screendump", filename=shot_end)

        def per_core():
            out = qmp.cmd("human-monitor-command",
                          **{"command-line": "xp /4gx 0x%x" % irqs_phys})["return"]
            return [int(v, 16) for line in out.strip().splitlines()
                    for v in line.split(":")[1].split()]
        text = open(log, errors="replace").read()
        ticks_before = per_core()
        time.sleep(TICK_WINDOW_S)
        ticks_after = per_core()
    finally:
        qemu.terminate()
        qemu.wait()

    fail = 0

    def check(ok, what):
        nonlocal fail
        fail |= not ok
        print("  [%s]  %s" % ("ok" if ok else "FAIL", what))

    for m in markers:
        check(m in text, m)

    w, h, px = read_ppm(shot_rawcon)
    fg, mark = count(px, FG), count(px, MARK)
    where = "at %s" % HANDOVER if reached_handover else "where the boot stopped"
    # A few hundred pixels of text is one short line; a real log is thousands.
    check(fg > 2000, "screen %dx%d %s: rawcon drew the kernel log (%d text pixels)"
          % (w, h, where, fg))
    check(mark > 0, "screen: rawcon's newest-line bar is BLUE -- red/blue not swapped (%d px)"
          % mark)
    online = [c for c in range(4) if "smp: cpu%d online" % c in text]
    if online:
        grew = [b - a for a, b in zip(ticks_before, ticks_after)]
        cores = [0] + online
        check(all(grew[c] > 0 for c in cores),
              "every online core's timer ticks: %s interrupts in %d s"
              % (", ".join("cpu%d +%d" % (c, grew[c]) for c in cores), TICK_WINDOW_S))
    if reached_handover:
        _, _, px = read_ppm(shot_end)
        n = bright(px)
        check(n > 2000, "screen at the end of the boot: the log is still on it, "
              "now from the console (%d text pixels)" % n)

    if fail:
        print("test-rpi4-boot: FAILED -- serial log in %s, screens in %s" % (log, tmp))
        sys.exit(1)
    print("test-rpi4-boot: passes on QEMU raspi4b  (docs/RPI4.md; log: %s)" % log)


if __name__ == "__main__":
    main()
