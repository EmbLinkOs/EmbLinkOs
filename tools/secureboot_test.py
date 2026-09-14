#!/usr/bin/env python3
"""secureboot_test.py -- will a machine with Secure Boot on actually run this?

    make test-secureboot

Retail hardware ships with Secure Boot enabled. A machine in that state does
not warn about an unsigned EFI application, negotiate, or fall back: the
firmware says "Access Denied" and moves to the next boot option, and somebody
who has just written this OS to a USB stick watches their laptop ignore it.

THE TEST IS A PAIR, and neither half means anything alone:

    unsigned  ->  MUST be refused    (proves the firmware is really enforcing)
    signed    ->  MUST boot          (proves our signature is really valid)

Only the first half makes the second one evidence. Running just the signed case
against a firmware that happens not to be enforcing produces a pass that says
nothing at all -- which is exactly what happened the first time this was tried
by hand: OVMF's stock variable store has no Platform Key, so it is in SETUP
MODE, and the unsigned loader booted straight to the desktop under the
"secure" firmware.

So the store gets a key enrolled into it first (tools/efivars.py), and the
firmware needs SMM for any of it to be enforced -- hence q35 and the secure
pflash, which is the configuration a real machine is in and not a special one.

WHAT IS BEING TRUSTED HERE, stated plainly: the key is generated on this
machine and enrolled by writing the firmware's flash image directly. That is
the emulator's equivalent of the owner standing in front of the machine with a
programmer, which is the position a computer's owner is supposed to be in. It
proves the signature is well-formed and the chain works. It does NOT make this
OS bootable on somebody else's laptop, where the enrolled keys are Microsoft's
-- that needs either their signature or the owner enrolling ours, and
docs/TODO.md says so rather than this test implying otherwise.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
KEYS = os.path.join(BUILD, "sbkeys")

FW_DIR = os.environ.get("QEMU_FW_DIR", "")
SECURE_CODE = os.environ.get("OVMF_SECURE_CODE", "")
VARS_TEMPLATE = os.environ.get("OVMF_VARS", "")

BOOT_SECONDS = int(os.environ.get("SB_BOOT_SECONDS", "90"))


def sh(*argv, **kw):
    r = subprocess.run(argv, capture_output=True, **kw)
    if r.returncode != 0:
        raise SystemExit("secureboot: %s failed:\n%s"
                         % (argv[0], r.stderr.decode("utf-8", "replace")))
    return r.stdout


def make_keys():
    """A platform key, generated once and kept in build/.

    NOT checked into the tree, and it must not be: a signing key in a public
    repository signs nothing meaningfully. It is regenerated on any machine
    that does not have one, which is right for a test key and exactly wrong
    for a release one -- see docs/TODO.md."""
    os.makedirs(KEYS, exist_ok=True)
    key = os.path.join(KEYS, "PK.key")
    crt = os.path.join(KEYS, "PK.crt")
    der = os.path.join(KEYS, "PK.der")
    if os.path.exists(key) and os.path.exists(crt) and os.path.exists(der):
        return key, crt, der
    print("secureboot: generating a platform key in %s"
          % os.path.relpath(KEYS, ROOT))
    sh("openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-sha256",
       "-days", "3650", "-nodes",
       "-subj", "/CN=EmbLinkOS Secure Boot Platform Key/",
       "-keyout", key, "-out", crt)
    sh("openssl", "x509", "-in", crt, "-outform", "DER", "-out", der)
    return key, crt, der


def boot(image, vars_file, log_path, want_desktop):
    """One boot under the Secure Boot firmware.

    SMM AND q35 ARE NOT OPTIONAL. OVMF's secure build keeps the variable store
    behind System Management Mode, and without `smm=on` plus the `secure`
    pflash property the firmware comes up with nothing enforced -- looking
    exactly like a pass."""
    if os.path.exists(log_path):
        os.remove(log_path)
    run_vars = log_path + ".vars"
    sh("cp", "-f", vars_file, run_vars)

    argv = [
        "qemu-system-x86_64",
        "-machine", "q35,smm=on",
        "-global", "driver=cfi.pflash01,property=secure,value=on",
        "-global", "ICH9-LPC.disable_s3=1",
        "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % SECURE_CODE,
        "-drive", "if=pflash,format=raw,unit=1,file=%s" % run_vars,
        "-drive", "format=raw,file=%s,if=ide,index=0" % image,
        "-drive", "format=raw,file=%s,if=ide,index=1" % os.path.join(ROOT, "embkfs.img"),
        "-serial", "file:%s" % log_path,
        "-display", "none", "-no-reboot", "-no-shutdown", "-m", "512M",
    ]
    p = subprocess.Popen(argv, cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        end = time.time() + (BOOT_SECONDS if want_desktop else 30)
        while time.time() < end:
            time.sleep(2)
            try:
                with open(log_path, "rb") as f:
                    d = f.read()
                # Either outcome is final; there is nothing to wait for after it.
                if b"desktop ready" in d or b"No bootable option" in d:
                    time.sleep(1)
                    break
            except OSError:
                pass
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()
    with open(log_path, "rb") as f:
        return f.read().decode("utf-8", "replace").replace("\r", "")


def main():
    if not SECURE_CODE or not os.path.exists(SECURE_CODE):
        print("secureboot: no Secure Boot firmware at %r -- set "
              "OVMF_SECURE_CODE (the Makefile resolves it per host)" % SECURE_CODE)
        return 2
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("secureboot: another qemu-system is running; refusing to start a second one")
        return 2

    key, crt, der = make_keys()

    enrolled = os.path.join(BUILD, "ovmf_sb_enrolled.fd")
    sh(sys.executable, os.path.join(ROOT, "tools", "efivars.py"),
       VARS_TEMPLATE, enrolled, der)

    signed_efi = os.path.join(BUILD, "BOOTX64.signed.efi")
    out = sh(sys.executable, os.path.join(ROOT, "tools", "sbsign.py"),
             os.path.join(BUILD, "BOOTX64.EFI"), signed_efi, key, crt)
    for line in out.decode().splitlines():
        print("  " + line)
    signed_img = os.path.join(BUILD, "uefi-signed.img")
    sh(sys.executable, os.path.join(ROOT, "tools", "mkuefidisk.py"),
       signed_efi, signed_img)

    fails = []

    # ---- half one: the UNSIGNED image must be refused --------------------
    print("=== secureboot: the unsigned loader (must be REFUSED)")
    out = boot(os.path.join(ROOT, "uefi.img"), enrolled,
               os.path.join(BUILD, "sb-unsigned.log"), want_desktop=False)
    denied = "Access Denied" in out
    booted = "desktop ready" in out
    print("  firmware said: %s" % ("Access Denied" if denied else
                                   "it launched the image"))
    if not denied:
        fails.append("the firmware did NOT refuse an unsigned loader -- Secure "
                     "Boot is not enforcing, so the signed result below would "
                     "prove nothing")
    if booted:
        fails.append("the unsigned loader reached the desktop")

    # ---- half two: the SIGNED image must boot ----------------------------
    print("=== secureboot: the signed loader (must BOOT)")
    out = boot(signed_img, enrolled,
               os.path.join(BUILD, "sb-signed.log"), want_desktop=True)
    if "Access Denied" in out:
        fails.append("the firmware refused our SIGNED loader -- the signature "
                     "is not valid for the key that is enrolled")
    if "desktop ready" not in out:
        fails.append("the signed loader did not reach the desktop")

    # AND THE OS HAS TO KNOW. A machine that boots under Secure Boot but
    # reports it as off is telling its user something false about itself.
    m = re.search(r"secure boot: ([^\n]+)", out)
    if not m:
        fails.append("the loader never reported the firmware's secure-boot state")
    else:
        state = m.group(1).strip()
        print("  the loader reported: secure boot: %s" % state)
        if not state.startswith("ENABLED"):
            fails.append("booted under enforcing Secure Boot but reported %r"
                         % state)
    if "kernel image: matches its build hash" not in out:
        fails.append("the loader did not verify the kernel it jumped to")

    for f in fails:
        print("  FAIL: " + f)
    print("=== test-secureboot: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
