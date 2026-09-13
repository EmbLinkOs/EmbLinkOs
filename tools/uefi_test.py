#!/usr/bin/env python3
"""uefi_test.py -- does this machine boot the way machines boot now?

    make test-uefi

Every x86 machine built in roughly the last decade starts its operating system
by having its firmware load an EFI application off a GPT disk. There is no BIOS
path on many of them at all. So this is not a compatibility mode: for the goal
this OS is held to -- installed on a separate physical machine and used every
day -- it is THE boot path, and the legacy one is the fallback.

The loader is ours (boot/uefi/, no GNU-EFI) and it has failed before in a way
that leaves nothing behind: a position-independent EFI image that does not
relocate ITSELF reads every global from low memory that is not the image, and
dies on its first console call with the firmware's screen still blank. That
bug was found by writing single bytes to COM1 and is caught here by reading
them back.

WHAT THIS CHECKS, and why each one is a separate line rather than "it booted":

  markers 1 2 3 4    the loader got through firmware handoff, its own
                     relocation, the MS x64 ABI, and storing the system table.
                     These are the only evidence a real machine with no
                     display would give, so they are asserted directly.
  the menu drew      through the firmware's ConOut, i.e. the console works
  marker B           the menu dispatched a boot
  a GOP framebuffer  a real address and real dimensions from the firmware's
                     graphics protocol -- not the VGA text mode the BIOS path
                     gets, which a UEFI machine may not have
  fw UEFI            the KERNEL agrees it was booted by UEFI, from the boot
                     protocol it was handed, rather than this test inferring it
  a memory map       translated from the firmware's, with a plausible number of
                     entries. Getting this wrong is a kernel that boots and
                     then corrupts itself minutes later
  root mounted       EMBKFS found and read
  desktop ready      userspace all the way to a session

TWO IMAGES, because they are different claims:

  uefi.img      loader on one disk, root filesystem on another. The developer
                arrangement.
  uefi-usb.img  ONE GPT disk: [ESP: loader+kernel] + [EMBKFS: root]. This is
                what you write to a USB stick and carry to the target machine,
                and it additionally proves the partition scan finds the root on
                the same device the firmware booted from -- which the two-disk
                image cannot prove, because there the root is the only
                filesystem on its disk.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

# The firmware. Its path is per host; the Makefile resolves the same two files
# and passes them in, so this agrees with `make run-uefi` by construction.
OVMF_CODE = os.environ.get("OVMF_CODE", "")
OVMF_VARS = os.environ.get("OVMF_VARS", "")

BOOT_SECONDS = int(os.environ.get("UEFI_BOOT_SECONDS", "75"))


def qemu_busy():
    return any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
               for e in ("qemu-system-x86_64", "qemu-system-aarch64"))


def boot(image, extra_drive, log_path):
    """One OVMF boot, serial to a file. The VARS store must be a writable
    per-run COPY -- the firmware writes its boot entries into it, and a run
    that mutated the shared one would make the next run different."""
    vars_copy = os.path.join(BUILD, "ovmf_vars_test.fd")
    subprocess.run(["cp", "-f", OVMF_VARS, vars_copy], check=True)
    if os.path.exists(log_path):
        os.remove(log_path)

    argv = [
        "qemu-system-x86_64",
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=%s" % vars_copy,
        "-drive", "format=raw,file=%s,if=ide,index=0" % image,
    ]
    if extra_drive:
        argv += ["-drive", "format=raw,file=%s,if=ide,index=1" % extra_drive]
    argv += ["-serial", "file:%s" % log_path, "-display", "none",
             "-no-reboot", "-no-shutdown", "-m", "512M"]

    p = subprocess.Popen(argv, cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Poll for the finish line rather than sleeping the worst case: a boot
        # that works takes about half this and a boot that does not is going to
        # be read either way.
        end = time.time() + BOOT_SECONDS
        while time.time() < end:
            time.sleep(2)
            try:
                with open(log_path, "rb") as f:
                    if b"desktop ready" in f.read():
                        time.sleep(1)
                        break
            except OSError:
                pass
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()

    with open(log_path, "rb") as f:
        # The firmware and the menu both write CR and ANSI escapes; strip the
        # CRs so line-oriented checks below are not defeated by formatting.
        return f.read().decode("utf-8", "replace").replace("\r", "")


def check(name, image, extra_drive, single_device):
    log_path = os.path.join(BUILD, "uefi-test-%s.log" % name)
    print("=== uefi-test: %s (%s)" % (name, os.path.basename(image)))
    out = boot(image, extra_drive, log_path)
    fails = []

    def want(cond, why):
        if not cond:
            fails.append(why)

    # THE LOADER'S OWN PROGRESS. In order, and contiguous: the four bytes are
    # written with nothing between them, so anything interleaved means a step
    # ran twice or the firmware wrote over us.
    want("1234" in out,
         "the loader's COM1 markers 1-2-3-4 are not all there -- it died "
         "before storing the system table (got %r)"
         % "".join(c for c in out[:400] if c in "1234B"))
    want("\nB\n" in out or "B\nBooting EmbLinkOS" in out,
         "marker B is missing -- the menu never dispatched a boot")

    want("EmbBoot" in out, "the menu never drew -- the firmware console is not usable")
    want("Booting EmbLinkOS" in out, "the boot entry never ran")

    # THE FIRMWARE'S GRAPHICS PROTOCOL, not a VGA text mode that may not exist.
    m = re.search(r"bootproto: fb ([0-9a-f]+) (\d+)x(\d+) pitch (\d+) bpp (\d+)", out)
    if not m:
        fails.append("the kernel never reported a framebuffer from the boot protocol")
    else:
        addr, w, h, pitch, bpp = (int(m.group(1), 16), int(m.group(2)),
                                  int(m.group(3)), int(m.group(4)), int(m.group(5)))
        print("  GOP framebuffer 0x%x %dx%d pitch %d bpp %d" % (addr, w, h, pitch, bpp))
        want(addr != 0, "the framebuffer address is 0 -- GOP was not located")
        want(w >= 640 and h >= 480, "framebuffer %dx%d is not a real mode" % (w, h))
        want(pitch >= w * (bpp // 8),
             "pitch %d cannot hold %d pixels of %d bpp" % (pitch, w, bpp))

    # THE KERNEL'S OWN ACCOUNT of how it was booted. This is the line that
    # distinguishes a UEFI boot from the BIOS path reaching the same desktop.
    want("bootproto: v1" in out and "fw UEFI" in out,
         "the kernel did not report being booted by UEFI")

    m = re.search(r"bootproto: mmap (\d+) entries", out)
    if not m:
        fails.append("no memory map was handed to the kernel")
    else:
        n = int(m.group(1))
        print("  memory map: %d entries translated from the firmware's" % n)
        want(4 <= n <= 512,
             "%d memory-map entries is not a plausible firmware map" % n)

    # THE ROOT FILESYSTEM, and on the single-device image, WHERE it is.
    m = re.search(r"EMBKFS: (sd\w+): root node OK", out)
    if not m:
        fails.append("the EMBKFS root was never mounted")
    else:
        dev = m.group(1)
        print("  root filesystem mounted on %s" % dev)
        if single_device:
            want(re.search(r"part: sda: GPT, (\d+) partition", out) is not None,
                 "no GPT partition table was found on the boot disk")
            want(dev.startswith("sda"),
                 "root came up on %s -- on a single-device image it must be a "
                 "partition of the disk the firmware booted from" % dev)

    want("init: desktop session started" in out, "no session was started")
    want("home: desktop ready" in out, "userspace never reached the desktop")

    for f in fails:
        print("     FAIL: %s" % f)
    print("=== uefi-test: %s %s  (%s)"
          % (name, "FAIL" if fails else "OK", log_path))
    return len(fails)


def main():
    if not OVMF_CODE or not os.path.exists(OVMF_CODE):
        print("uefi-test: no OVMF firmware at %r -- set OVMF_CODE/OVMF_VARS "
              "(the Makefile resolves them per host)" % OVMF_CODE)
        return 2
    if qemu_busy():
        print("uefi-test: another qemu-system is running; refusing to start a second one")
        return 2

    bad = 0
    bad += check("two-disk", os.path.join(ROOT, "uefi.img"),
                 os.path.join(ROOT, "embkfs.img"), single_device=False)
    if qemu_busy():
        print("uefi-test: a qemu is still running; not starting the second boot")
        return 2
    bad += check("usb", os.path.join(ROOT, "uefi-usb.img"),
                 None, single_device=True)

    print("=== test-uefi: %s" % ("FAIL (%d)" % bad if bad else "OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
