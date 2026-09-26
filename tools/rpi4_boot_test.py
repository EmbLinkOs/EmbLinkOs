#!/usr/bin/env python3
"""Boot the Raspberry Pi 4 kernel on QEMU raspi4b and check what it claims.

    tools/rpi4_boot_test.py <kernel.img> <bcm2711-rpi-4-b.dtb> <log> <marker>...

Used by `make ARCH=aarch64 BOARD=rpi4 test-rpi4-boot` -- docs/RPI4.md.

Two witnesses, because a Pi on a television has two outputs:

  * the SERIAL log, one grep per marker the current phase claims;
  * the SCREEN, taken with QMP `screendump` once the boot has gone as far as it
    goes, and checked for the kernel log actually being drawn there: text in
    rawcon's foreground colour, and rawcon's blue "newest line" bar. The bar is
    the byte-order check -- with red and blue swapped it comes out orange, and
    the serial log cannot tell you that.

It waits for the kernel's last word (today: the GIC line that ends P0) rather
than a fixed time, so a slow host takes longer instead of failing.
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

END_OF_BOOT = ("gic: FATAL", "=== aarch64 exception ===")
TIMEOUT_S = 120


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def screendump(port, path):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        f = s.makefile("rw")
        f.readline()                                   # the greeting
        for cmd in ({"execute": "qmp_capabilities"},
                    {"execute": "screendump", "arguments": {"filename": path}}):
            f.write(json.dumps(cmd) + "\n")
            f.flush()
            reply = json.loads(f.readline())
            if "error" in reply:
                raise RuntimeError(reply["error"])


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


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    kernel, dtb, log = sys.argv[1:4]
    markers = sys.argv[4:]

    port = free_port()
    if os.path.exists(log):
        os.remove(log)
    qemu = subprocess.Popen(
        ["qemu-system-aarch64", "-M", "raspi4b", "-dtb", dtb,
         "-display", "none", "-monitor", "none",
         "-serial", "file:" + log,
         "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port,
         "-kernel", kernel],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    shot = os.path.join(tempfile.mkdtemp(), "screen.ppm")
    try:
        deadline = time.time() + TIMEOUT_S
        text = ""
        while time.time() < deadline:
            text = open(log, errors="replace").read() if os.path.exists(log) else ""
            if any(m in text for m in END_OF_BOOT):
                break
            time.sleep(1)
        time.sleep(1)                                  # let the last line draw
        screendump(port, shot)
        text = open(log, errors="replace").read()
    finally:
        qemu.terminate()
        qemu.wait()

    fail = 0
    for m in markers:
        ok = m in text
        fail |= not ok
        print("  [%s]  %s" % ("ok" if ok else "FAIL", m))

    w, h, px = read_ppm(shot)
    fg, mark = count(px, FG), count(px, MARK)
    # A few hundred pixels of text is one short line; a real log is thousands.
    ok = fg > 2000
    fail |= not ok
    print("  [%s]  screen %dx%d: kernel log drawn (%d text pixels)"
          % ("ok" if ok else "FAIL", w, h, fg))
    ok = mark > 0
    fail |= not ok
    print("  [%s]  screen: newest-line bar is BLUE -- red/blue not swapped (%d px)"
          % ("ok" if ok else "FAIL", mark))

    if fail:
        print("test-rpi4-boot: FAILED -- serial log in %s, screen in %s" % (log, shot))
        sys.exit(1)
    print("test-rpi4-boot: passes on QEMU raspi4b  (docs/RPI4.md; log: %s)" % log)


if __name__ == "__main__":
    main()
