#!/usr/bin/env python3
"""Does the root filesystem survive losing the one block every lookup passes through?

On an HP ProBook, one marginal flash block -- EMBKFS node block 17, the root
of the metadata tree -- made a freshly written root filesystem unreadable:
every read of it failed with I/O error, every lookup needed it, and the
machine looped on the first file init tried to open. The backup superblock
existed; the root node had no backup.

Now it does (v2.3.1): mkfs writes a byte-identical copy of the root at the far
end of the volume and names it in the superblock, every commit writes the new
root twice, and a read of the root that fails -- I/O error or bad checksum --
is served from the mirror, with a line in the log saying so.

Three boots of the same image, one per case, with the root filesystem as the
second IDE disk (the arrangement tools/usb_hotplug.py uses):

  control   the image as built: the mirror line must NOT appear, and the
            root listing must be there (nothing regressed for a healthy disk);
  garbage   block 17 overwritten with noise: checksum failure, served from
            the mirror, root listing present;
  ioerror   QEMU's blkdebug injects errno 5 on every read touching sector 136
            (block 17, LBA 136 -- the ProBook's exact failure): I/O error,
            served from the mirror, root listing present.

Run: python3 tools/root_mirror_test.py   (make test-root-mirror)
"""
import os
import re
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemu_guard  # noqa: E402

SOCK = os.path.join(BUILD, "rootmirror-serial.sock")
BLOCK = 4096
SB_OFFSET = 65536
MIRROR_LINE = "using its mirror"


def root_block_of(img_path):
    """The root node's block number, from the primary superblock (root ptr @88)."""
    with open(img_path, "rb") as f:
        f.seek(SB_OFFSET + 88)
        return struct.unpack("<Q", f.read(8))[0]


def boot(drive_args, tag, seconds=75):
    """Boot once; return the console text."""
    if os.path.exists(SOCK):
        os.remove(SOCK)
    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
    ] + drive_args + [
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "rootmirror-%s-qemu.log" % tag), "wb")
    p = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
    buf = bytearray()
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
            raise SystemExit("root-mirror: the guest never opened its serial socket")
        s.settimeout(0.4)
        end = time.time() + seconds
        while time.time() < end:
            try:
                b = s.recv(65536)
                if not b:
                    break
                buf.extend(b)
            except Exception:
                pass
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()
        log.close()
    text = buf.decode("utf-8", "replace")
    open(os.path.join(BUILD, "rootmirror-%s-console.log" % tag), "w").write(text)
    return text


def listing_entries(text):
    m = re.findall(r"ls: (\d+) entr", text)
    return int(m[0]) if m else -1


def main():
    if qemu_guard.other_qemu_running():
        print("root-mirror: another qemu-system is running; refusing to start a second one")
        return 2

    master = os.path.join(ROOT, "embkfs.img")
    rb = root_block_of(master)
    print("root-mirror: the root node is block %d (LBA %d)" % (rb, rb * 8))
    fails = []

    def expect(tag, text, want_mirror, want_error=None):
        n = listing_entries(text)
        mirrored = MIRROR_LINE in text
        print("root-mirror: %-8s root listing %s entries, mirror line %s"
              % (tag, n if n >= 0 else "MISSING", "present" if mirrored else "absent"))
        if n < 5:
            fails.append("%s: the root directory was not listed (ls: %s)" % (tag, n))
        if want_mirror and not mirrored:
            fails.append("%s: the root was damaged but the log never said '%s'" % (tag, MIRROR_LINE))
        if not want_mirror and mirrored:
            fails.append("%s: a healthy root was served from the mirror" % tag)
        if want_error and not re.search(want_error, text):
            fails.append("%s: expected the log to show /%s/ first" % (tag, want_error))

    # ---- control --------------------------------------------------------
    scratch = os.path.join(BUILD, "rootmirror-control.img")
    subprocess.run(["cp", "-f", master, scratch], check=True)
    text = boot(["-drive", "format=raw,file=%s,if=ide,index=1" % scratch], "control")
    expect("control", text, want_mirror=False)

    # ---- garbage root ---------------------------------------------------
    scratch = os.path.join(BUILD, "rootmirror-garbage.img")
    subprocess.run(["cp", "-f", master, scratch], check=True)
    with open(scratch, "r+b") as f:
        f.seek(rb * BLOCK)
        f.write(bytes((i * 7 + 13) & 0xFF for i in range(BLOCK)))
    text = boot(["-drive", "format=raw,file=%s,if=ide,index=1" % scratch], "garbage")
    # Noise fails the MAGIC check, which the reader makes before the checksum;
    # a block that kept its magic would fail the checksum instead. Either is
    # the primary rejecting the block, which is what the mirror is for.
    expect("garbage", text, want_mirror=True, want_error="bad node magic|checksum bad")

    # ---- I/O error on the root's sectors --------------------------------
    scratch = os.path.join(BUILD, "rootmirror-ioerror.img")
    subprocess.run(["cp", "-f", master, scratch], check=True)
    cfg = os.path.join(BUILD, "rootmirror-blkdebug.cfg")
    with open(cfg, "w") as f:
        f.write('[inject-error]\nevent = "read_aio"\nsector = "%d"\nerrno = "5"\n'
                'once = "off"\n' % (rb * 8))
    text = boot(["-drive", "if=ide,index=1,format=raw,"
                 "file.driver=blkdebug,file.config=%s,"
                 "file.image.driver=file,file.image.filename=%s" % (cfg, scratch)], "ioerror")
    expect("ioerror", text, want_mirror=True, want_error="I/O error")

    for f in fails:
        print("  FAIL: %s" % f)
    print("=== test-root-mirror: %s" % ("OK" if not fails else "FAIL"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
