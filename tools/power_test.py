#!/usr/bin/env python3
"""power_test.py -- does the machine actually turn off, and actually reboot,
the way its OWN ACPI tables say to?

    python3 tools/power_test.py            # both chipsets, both actions
    MACHINES=q35 python3 tools/power_test.py

Boots the x86 kernel on each chipset QEMU emulates -- the older PIIX4 PC and
the ICH9/Q35 one, which carry DIFFERENT DSDTs -- waits for the desktop, types
`poweroff` (or `reboot`) at the serial console, and judges three things:

  1. the kernel found the FADT and decoded \\_S5_ from the AML;
  2. it switched the machine off through THAT (the "ACPI S5 via" line) and
     never fell through to the emulator constants -- a power-off that works
     only because the fallback guessed right proves nothing about a real PC;
  3. QEMU itself exited, which is the only evidence the power really went.

Reboot is judged against what the FADT DECLARES: through the ACPI reset
register when the table has one, through the 8042 when it does not (an ACPI
1.0 table cannot), and in both cases QEMU exiting -- -no-reboot turns a reset
into an exit.

ONE QEMU AT A TIME, like console_test.py.
"""
import os, sys, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(machine, action):
    argv = ["qemu-system-x86_64", "-M", machine, "-cpu", "max", "-m", "2G", "-smp", "4",
            "-display", "none", "-serial", "stdio", "-no-reboot",
            "-drive", "format=raw,file=%s/myos.img,if=ide,index=0" % ROOT,
            "-drive", "format=raw,file=%s/embkfs.img,if=ide,index=1" % ROOT]
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)
    buf, sent, t0 = b"", None, time.time()
    while time.time() - t0 < 300:
        try:
            c = p.stdout.read()
            if c:
                buf += c
        except Exception:
            pass
        if sent is None and (b"first frame presented" in buf or b"desktop ready" in buf):
            time.sleep(3)
            p.stdin.write(action.encode() + b"\r")
            p.stdin.flush()
            sent = time.time()
        if p.poll() is not None:
            break
        if sent is not None and time.time() - sent > 60:
            break
        time.sleep(0.2)
    exited = p.poll() is not None
    if not exited:
        p.kill()
    out = buf.decode(errors="replace")
    keep = [l for l in out.splitlines()
            if l.startswith("ACPI:") or l.startswith("power:") or l.strip() in ("poweroff", "reboot")]

    s5_parsed = "power-off is real" in out
    if action == "poweroff":
        via_acpi = "ACPI S5 via" in out
        fallback = "trying QEMU" in out or "ACPI S5 did not take" in out
        ok = s5_parsed and via_acpi and not fallback and exited
    else:
        # Judged against what the machine DECLARES. An ACPI 1.0 FADT has no
        # reset register (QEMU's PIIX4 PC ships one), and there the 8042 is
        # the correct mechanism, not a fallback. Where the FADT does declare
        # one, using anything else is the failure.
        declared = "reset not supported" not in out
        via_acpi = "rebooting via the ACPI reset register" in out
        fell_past = "did not reset the machine" in out
        if declared:
            ok = via_acpi and not fell_past and exited
        else:
            ok = ("8042" in out) and exited
    return ok, exited, keep


def main():
    # EXACT executable name, not a command-line substring. `pgrep -f
    # qemu-system` matches ANY process whose command line contains that text --
    # including a shell one-liner that waits for qemu to exit, which is exactly
    # what a person or a script babysitting a long run will be running. It cost
    # a real debugging session: every tool here refused to start, reporting
    # "another qemu-system is running", while nothing was running but the
    # waiters themselves. -x matches the program, which is the question.
    if any(subprocess.run(["pgrep", "-x", exe], capture_output=True).returncode == 0
           for exe in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("power_test: another qemu-system is running; refusing to start a second one")
        return 2
    machines = os.environ.get("MACHINES", "pc q35").split()
    fails = 0
    for m in machines:
        for action in ("poweroff", "reboot"):
            ok, exited, lines = run(m, action)
            print("=== -M %s, %s: %s" % (m, action, "OK" if ok else "FAIL"))
            for l in lines:
                print("    " + l)
            if not exited:
                print("    (QEMU was still running 60 s after the command)")
            fails += 0 if ok else 1
    print("power_test: %s" % ("OK" if fails == 0 else "%d FAILED" % fails))
    return fails


if __name__ == "__main__":
    sys.exit(main())
