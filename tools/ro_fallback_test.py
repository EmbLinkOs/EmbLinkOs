#!/usr/bin/env python3
"""When the disk refuses writes, does the machine still reach a usable desktop?

On an HP ProBook the stick read perfectly and wedged on the first write. With
a real transfer timeout the kernel waited 30 s for that write, then 30 s for
the next command, and would have gone on: every one of init's eight mkdirs,
then every write the desktop makes, each a half-minute of nothing.

A volume whose device fails a write is now read-only from that moment: the
failed commit flips the flag, every later write fails at once with EROFS, and
the boot goes on -- init's mkdirs fail fast, the desktop is spawned, the root
is listable. The log says so once.

Two boots of the same image as the second IDE disk:

  control   nothing injected: no read-only line, root listed;
  wrfail    QEMU's blkdebug fails EVERY write with errno 5 (reads untouched):
            the first commit fails with I/O error, the read-only line appears,
            later writes say "Read-only file system" rather than "I/O error",
            and the root is still listed.

Run: python3 tools/ro_fallback_test.py   (make test-ro-fallback)
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemu_guard  # noqa: E402

SOCK = os.path.join(BUILD, "rofb-serial.sock")
RO_LINE = "read-only from now on"


def boot(drive_args, tag, seconds=75):
    if os.path.exists(SOCK):
        os.remove(SOCK)
    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
    ] + drive_args + [
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        # The live session: init makes the account's directories on the way to
        # the desktop -- the later writes that must fail at once with EROFS.
        "-fw_cfg", "name=opt/emblink/live,string=1",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "rofb-%s-qemu.log" % tag), "wb")
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
            raise SystemExit("ro-fallback: the guest never opened its serial socket")
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
    open(os.path.join(BUILD, "rofb-%s-console.log" % tag), "w").write(text)
    return text


def entries(text):
    m = re.findall(r"ls: (\d+) entr", text)
    return int(m[0]) if m else -1


def main():
    if qemu_guard.other_qemu_running():
        print("ro-fallback: another qemu-system is running; refusing to start a second one")
        return 2
    master = os.path.join(ROOT, "embkfs.img")
    fails = []

    # ---- control ----------------------------------------------------------
    scratch = os.path.join(BUILD, "rofb-control.img")
    subprocess.run(["cp", "-f", master, scratch], check=True)
    text = boot(["-drive", "format=raw,file=%s,if=ide,index=1" % scratch], "control")
    n = entries(text)
    print("ro-fallback: control  root listing %s, read-only line %s"
          % (n, "present" if RO_LINE in text else "absent"))
    if n < 5:
        fails.append("control: the root directory was not listed")
    if RO_LINE in text:
        fails.append("control: a healthy disk was declared read-only")

    # ---- every write fails ----------------------------------------------
    scratch = os.path.join(BUILD, "rofb-wrfail.img")
    subprocess.run(["cp", "-f", master, scratch], check=True)
    cfg = os.path.join(BUILD, "rofb-blkdebug.cfg")
    with open(cfg, "w") as f:
        # iotype: without it the rule, once a write has fired its event, fails
        # READS too -- and a disk that cannot be read is not the case under test.
        f.write('[inject-error]\nevent = "write_aio"\niotype = "write"\nerrno = "5"\nonce = "off"\n')
    text = boot(["-drive", "if=ide,index=1,format=raw,"
                 "file.driver=blkdebug,file.config=%s,"
                 "file.image.driver=file,file.image.filename=%s" % (cfg, scratch)], "wrfail")
    n = entries(text)
    io_errors = len(re.findall(r"I/O error", text))
    erofs = len(re.findall(r"Read-only file system|read-only file system", text))
    print("ro-fallback: wrfail   root listing %s, read-only line %s, "
          "'I/O error' x%d, 'Read-only file system' x%d"
          % (n, "present" if RO_LINE in text else "absent", io_errors, erofs))
    if n < 5:
        fails.append("wrfail: the root directory was not listed -- reads should still work")
    if RO_LINE not in text:
        fails.append("wrfail: the first failed write did not make the volume read-only")
    if erofs < 1:
        fails.append("wrfail: no later write failed with EROFS -- they were still hitting the device")
    if io_errors > 4:
        fails.append("wrfail: %d I/O errors logged -- writes kept reaching a device that fails them"
                     % io_errors)
    if "could not spawn the confined desktop" in text:
        fails.append("wrfail: init gave up on the desktop")

    for f in fails:
        print("  FAIL: %s" % f)
    print("=== test-ro-fallback: %s" % ("OK" if not fails else "FAIL"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
