#!/usr/bin/env python3
"""Does a USB keyboard on xHCI still type when the controller's interrupt never arrives?

On the HP ProBook a USB keyboard works in the firmware and stops the moment
this kernel owns the controller. HID input on xHCI is delivered by the
controller's interrupt, and a laptop's interrupt routing is the kind of thing
that is wrong on a machine nobody has booted before. The kernel now counts
those interrupts and watches for evidence of one that did not come -- an event
left unconsumed at the ring's dequeue pointer across two polls -- and then
masks the interrupt for good and services the event ring from the main loop.
(Silence is not evidence: an idle keyboard raises no interrupt at all.)

This proves that path in QEMU, where the interrupt DOES work, by switching it
off on purpose:

  1. boot with a USB keyboard on qemu-xhci; type over QMP; the report counter
     must rise -- the interrupt path, as a baseline;
  2. `test xhci nointr` on the serial console: the controller's interrupt is
     masked and its counter reset, exactly the state a laptop with broken
     routing is in;
  3. type again: the first key leaves an event waiting, the fallback engages
     on that evidence, the counter must rise AGAIN, and the log must carry
     the line naming the fallback.

Run: python3 tools/xhci_nointr_test.py  (make test-xhci-nointr)
"""
import json
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

SOCK = os.path.join(BUILD, "xhcini-serial.sock")
QMP = os.path.join(BUILD, "xhcini-qmp.sock")

SETTLE_S = 1.0         # after masking: let the console line land before typing
KEYS = ["a", "b", "c", "d"]


class Qmp:
    def __init__(self, path):
        for _ in range(60):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.5)
        self.f = self.s.makefile("rwb", buffering=0)
        json.loads(self.f.readline())            # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, _command, **args):
        self.f.write((json.dumps({"execute": _command, "arguments": args}) + "\n").encode())
        while True:
            r = json.loads(self.f.readline())
            if "return" in r:
                return r["return"]
            if "error" in r:
                raise SystemExit("xhci-nointr: QMP error: %s" % r["error"])


def main():
    if qemu_guard.other_qemu_running():
        print("xhci-nointr: another qemu-system is running; refusing to start a second one")
        return 2
    for p in (SOCK, QMP):
        if os.path.exists(p):
            os.remove(p)
    scratch = os.path.join(BUILD, "xhcini-root.img")
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch], check=True)

    argv = [
        "qemu-system-x86_64", "-cpu", "max",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-kbd,bus=xhci.0",
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-no-reboot", "-no-shutdown", "-m", "1G", "-smp", "2",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "xhcini-qemu.log"), "wb")
    p = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)

    fails = []
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
            raise SystemExit("xhci-nointr: the guest never opened its serial socket")
        s.settimeout(0.4)
        buf = bytearray()
        everything = bytearray()          # the whole console, never cleared

        def drain(seconds):
            end = time.time() + seconds
            while time.time() < end:
                try:
                    b = s.recv(65536)
                    if not b:
                        break
                    buf.extend(b)
                    everything.extend(b)
                except Exception:
                    pass

        def stats(tag):
            buf.clear()
            s.sendall(b"test xhci stats\n")
            drain(3)
            text = buf.decode("utf-8", "replace")
            m = re.findall(r"\[xhci\] irqs=(\d+) kbd_reports=(\d+) mouse_reports=(\d+) "
                           r"polled_fallback=(\d)", text)
            if not m:
                fails.append("%s: the guest never answered `test xhci stats`" % tag)
                return None
            irqs, kbd, mouse, polled = (int(x) for x in m[-1])
            print("xhci-nointr: %-18s irqs=%d kbd_reports=%d polled_fallback=%d"
                  % (tag, irqs, kbd, polled))
            return irqs, kbd, mouse, polled

        def type_keys():
            q = Qmp(QMP)
            for k in KEYS:
                q.cmd("human-monitor-command", **{"command-line": "sendkey " + k})
                time.sleep(0.15)

        drain(70)                                   # let the boot settle
        before = stats("baseline")
        type_keys()
        time.sleep(1.0)
        after = stats("after typing")
        if before and after:
            if after[1] <= before[1]:
                fails.append("INTERRUPT PATH: %d keys typed, kbd_reports %d -> %d -- the "
                             "baseline does not even work" % (len(KEYS), before[1], after[1]))
            if after[3] != 0:
                fails.append("the fallback engaged while the interrupt was working (irqs=%d)"
                             % after[0])

        # Now the laptop's situation: the interrupt never comes.
        buf.clear()
        s.sendall(b"test xhci nointr\n")
        drain(2)
        time.sleep(SETTLE_S)
        mid = stats("interrupt masked")
        type_keys()
        time.sleep(1.0)
        final = stats("after typing again")
        if mid and final:
            if final[3] != 1:
                fails.append("the polled fallback did not engage (polled_fallback=%d)" % final[3])
            if final[1] <= mid[1]:
                fails.append("POLLED PATH: %d keys typed with the interrupt off, kbd_reports "
                             "%d -> %d" % (len(KEYS), mid[1], final[1]))
            if final[0] != mid[0]:
                fails.append("interrupts still arrived after masking (%d -> %d)" % (mid[0], final[0]))
        # The line a person would search the boot log for. It was printed the
        # moment the fallback engaged, which may have been during the boot
        # drain -- so the whole console is searched, not the last exchange.
        drain(1)
        if "servicing by polling" not in everything.decode("utf-8", "replace"):
            fails.append("the log never said 'servicing by polling'")
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    for f in fails:
        print("  FAIL: %s" % f)
    print("=== test-xhci-nointr: %s" % ("OK" if not fails else "FAIL"))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
