#!/usr/bin/env python3
"""install_test.py -- put the OS on a blank disk, then boot the blank disk.

    make test-install

docs/PILLARS.md phase 2: "There is no way to put the OS onto the target's
disk." You could build a USB stick on a development machine and boot from it,
and that was all. An operating system you cannot install is a demonstration.

THE TEST IS THE SECOND BOOT, and nothing before it counts. An installer that
writes plausible bytes, reports success and produces a disk the firmware will
not boot has failed in the only way that matters, and every check short of
actually booting the result would pass it.

So:

    1. boot from the UEFI stick image, with a BLANK second disk attached
    2. run the installer, target the blank disk
    3. shut the machine down, THROW THE STICK AWAY
    4. boot with only the installed disk, through the real firmware
    5. require a desktop

Step 3 is the one that makes it evidence. Leaving the source attached would
let the firmware boot from it and the test would pass with the target disk
completely empty.
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TARGET = os.path.join(BUILD, "installed.img")
SOCK = os.path.join(BUILD, "inst-ser.sock")

OVMF_CODE = os.environ.get("OVMF_CODE", "")
OVMF_VARS = os.environ.get("OVMF_VARS", "")

# Big enough to hold the source image with room over, so the test also
# exercises rebuilding the backup GPT at a DIFFERENT last-LBA than the source's
# -- which is the one part of the layout that cannot simply be copied.
TARGET_MB = int(os.environ.get("INSTALL_TARGET_MB", "320"))


def qemu_busy():
    return any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
               for e in ("qemu-system-x86_64", "qemu-system-aarch64"))


def vars_copy(tag):
    dst = os.path.join(BUILD, "ovmf_inst_%s.fd" % tag)
    subprocess.run(["cp", "-f", OVMF_VARS, dst], check=True)
    return dst


def run(argv, log_path, seconds, until=b"desktop ready"):
    if os.path.exists(log_path):
        os.remove(log_path)
    p = subprocess.Popen(argv, cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p


def drain_until(log_path, needle, seconds):
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(2)
        try:
            with open(log_path, "rb") as f:
                d = f.read()
            if needle in d:
                return d
        except OSError:
            pass
    try:
        with open(log_path, "rb") as f:
            return f.read()
    except OSError:
        return b""


def main():
    if not OVMF_CODE or not os.path.exists(OVMF_CODE):
        print("install-test: no OVMF firmware -- set OVMF_CODE/OVMF_VARS")
        return 2
    if qemu_busy():
        print("install-test: another qemu-system is running; refusing to start a second one")
        return 2

    src = os.path.join(ROOT, "uefi-usb.img")
    if not os.path.exists(src):
        print("install-test: %s is missing (make uefi-usb.img)" % src)
        return 2

    # A BLANK disk. Zeroed deliberately rather than reused: a target with an
    # old partition table on it could boot from leftovers and look installed.
    print("install-test: creating a blank %d MB target" % TARGET_MB)
    with open(TARGET, "wb") as f:
        f.truncate(TARGET_MB * 1024 * 1024)

    fails = []

    # ---- phase one: boot the stick, install onto the blank disk ----------
    print("=== install-test: booting the stick with a blank disk attached")
    log1 = os.path.join(BUILD, "install-phase1.log")
    if os.path.exists(SOCK):
        os.remove(SOCK)
    p = run([
        "qemu-system-x86_64",
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=%s" % vars_copy("p1"),
        "-drive", "format=raw,file=%s,if=ide,index=0" % src,
        "-drive", "format=raw,file=%s,if=ide,index=1" % TARGET,
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-display", "none", "-no-reboot", "-no-shutdown", "-m", "1G",
    ], log1, 0)

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
            raise SystemExit("install-test: the guest never opened its serial socket")
        s.settimeout(0.4)
        buf = bytearray()

        def drain(seconds):
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                except Exception:
                    pass
                if b"install: DONE" in buf or b"install: " in buf and b"\ninstall: " in buf:
                    pass

        drain(80)

        # WHICH DISK IS WHICH is decided by the guest, not assumed here: the
        # source is whatever the root filesystem came up on.
        buf.clear()
        s.sendall(b"run /data/apps/install/install.elf\n")
        drain(10)
        listing = buf.decode("utf-8", "replace")
        print("install-test: the guest sees:")
        for line in listing.splitlines():
            if line.strip().startswith("[") or "block device" in line:
                print("   " + line.strip())

        disks = re.findall(r"\[(\d+)\]\s+(\S+)\s+(\d+) MB\s+(whole disk|partition)",
                           listing)
        whole = [(n, sz) for _, n, sz, kind in disks if kind == "whole disk"]
        if len(whole) < 2:
            fails.append("the guest did not see two whole disks (saw %r)" % whole)
            raise SystemExit(0)

        # The target is the whole disk with no partitions under it -- the blank
        # one. The source has partitions, so its name appears as a prefix of
        # other entries.
        names = [n for n, _ in whole]
        parts = [n for _, n, _, kind in disks if kind == "partition"]
        target = None
        source = None
        for n in names:
            if any(p.startswith(n) for p in parts):
                source = n
            else:
                target = n
        if not source or not target:
            fails.append("could not tell the source disk from the target "
                         "(whole=%r partitions=%r)" % (names, parts))
            raise SystemExit(0)
        print("install-test: installing %s -> %s" % (source, target))

        buf.clear()
        s.sendall(("run /data/apps/install/install.elf %s %s\n"
                   % (source, target)).encode())
        end = time.time() + 300
        while time.time() < end:
            try:
                b = s.recv(65536)
                if b:
                    buf.extend(b)
            except Exception:
                pass
            if b"install: DONE" in buf or b"install: " in buf and b"failed" in buf:
                break
        out = buf.decode("utf-8", "replace")
        for line in out.splitlines():
            if line.strip().startswith("install:"):
                print("   " + line.strip())
        if "install: DONE" not in out:
            fails.append("the installer did not report DONE")

        # AND IT HAS TO HAVE GROWN WHAT IT COPIED. Installing from a 200 MB
        # stick onto a 320 MB disk and using 200 MB of it is the behaviour
        # this test exists to stop being acceptable -- it is not a failure the
        # second boot would catch, because a small filesystem boots perfectly.
        if "install: grew partition" not in out:
            fails.append("the installer did not grow the last partition to "
                         "fill the target")
        if "install: grew the filesystem" not in out:
            fails.append("the partition grew and the filesystem inside it did "
                         "not -- the extra space is unreachable")
        m = re.search(r"install: grew the filesystem from (\d+) MB to (\d+) MB", out)
        if m:
            before_mb, after_mb = int(m.group(1)), int(m.group(2))
            if after_mb <= before_mb:
                fails.append("the filesystem did not actually get bigger "
                             "(%d -> %d MB)" % (before_mb, after_mb))
            else:
                globals()["GREW_TO_MB"] = after_mb
                print("install-test: filesystem grown %d MB -> %d MB"
                      % (before_mb, after_mb))
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    if fails:
        for f in fails:
            print("  FAIL: " + f)
        print("=== test-install: FAIL")
        return 1

    # ---- phase two: boot the TARGET ALONE --------------------------------
    #
    # And read the size back from the machine that mounted it. The installer
    # SAYING it grew the filesystem and the kernel AGREEING when it next mounts
    # it are different claims: a superblock whose checksum no longer matches is
    # refused, and one whose total is bigger than the partition is worse than
    # that -- it allocates off the end.
    #
    # The source is not attached. If the installer wrote nothing, there is
    # nothing here to boot and the firmware says so.
    print("=== install-test: booting the installed disk ALONE (no stick)")
    log2 = os.path.join(BUILD, "install-phase2.log")
    p = run([
        "qemu-system-x86_64",
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=%s" % vars_copy("p2"),
        "-drive", "format=raw,file=%s,if=ide,index=0" % TARGET,
        "-serial", "file:%s" % log2,
        "-display", "none", "-no-reboot", "-no-shutdown", "-m", "1G",
    ], log2, 0)
    try:
        d = drain_until(log2, b"desktop ready", 120)
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    text = d.decode("utf-8", "replace").replace("\r", "")
    if "EmbBoot" not in text:
        fails.append("the firmware did not launch our loader from the installed "
                     "disk -- nothing bootable was written")
    m = re.search(r"EMBKFS: (sd\w+): root node OK", text)
    if not m:
        fails.append("the installed disk has no readable EMBKFS root")
    else:
        print("install-test: the installed disk mounted its root on %s" % m.group(1))
    if "desktop ready" not in text:
        fails.append("the installed disk did not reach a desktop")

    # THE KERNEL'S OWN READING OF THE SIZE. The mount banner prints the
    # superblock's totals, so this is the volume as the machine that just
    # mounted it understands it -- not as the installer claimed.
    m = re.search(r"EMBKFS: sd\w+: mounted.*?block_size (\d+)\s+blocks (\d+)", text)
    grew_to = globals().get("GREW_TO_MB")
    if not m:
        fails.append("the installed disk's mount banner did not report a size")
    elif grew_to:
        mounted_mb = (int(m.group(1)) * int(m.group(2))) >> 20
        print("install-test: the installed root mounted as %d MB" % mounted_mb)
        # Within a megabyte: the installer counts the partition, the
        # filesystem counts whole blocks of it, and the last partial block is
        # not usable.
        if abs(mounted_mb - grew_to) > 1:
            fails.append("the installer grew the filesystem to %d MB and the "
                         "kernel mounted %d MB -- the superblock and the "
                         "partition disagree" % (grew_to, mounted_mb))

    for f in fails:
        print("  FAIL: " + f)
    print("=== test-install: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
