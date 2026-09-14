#!/usr/bin/env python3
"""hotplug_test.py -- add a processor and a memory stick to a running machine.

    make test-hotplug

`test amldump` has shown \\_SB.CPUS with an _EJ0 method on every processor
since the AML interpreter was written, and the DSDT declares the memory
devices too. The firmware has been offering both the whole time and nothing
listened. docs/HARDWARE_GAPS.md section B: "the firmware is offering it and
nothing listens."

Same division of labour as the USB hot-plug test, for the same reason: the
guest cannot honestly claim a processor appeared, and the host that added it
can. So the guest REPORTS -- cores online, free pages -- and this side checks
those numbers move when it adds hardware.

    2 cores  ->  add a CPU  ->  3 cores online
    N pages  ->  add a DIMM ->  more than N

The second one is the stricter of the two. A CPU coming online is visible
because it says so; memory arriving is only real if the ALLOCATOR grew, which
means the page bitmap had to be extended past the end the firmware's memory
map described at boot.
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
SOCK = os.path.join(BUILD, "hotplug-ser.sock")
QMP = os.path.join(BUILD, "hotplug-qmp.sock")


class Qmp:
    def __init__(self, path):
        for _ in range(60):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(1)
        else:
            raise SystemExit("hotplug: QMP never came up")
        self.f = self.s.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, _command, **args):
        msg = {"execute": _command}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("hotplug: QMP closed")
            r = json.loads(line)
            if "event" in r:
                continue
            if "error" in r:
                raise SystemExit("hotplug: QMP error: %s" % r["error"])
            return r


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("hotplug: another qemu-system is running; refusing to start a second one")
        return 2

    for p in (SOCK, QMP):
        if os.path.exists(p):
            os.remove(p)
    scratch = os.path.join(BUILD, "hotplug-root.img")
    subprocess.run(["cp", "-f", os.path.join(ROOT, "embkfs.img"), scratch], check=True)

    argv = [
        "qemu-system-x86_64", "-cpu", "max", "-machine", "q35",
        "-drive", "format=raw,file=myos.img,if=ide,index=0",
        "-drive", "format=raw,file=%s,if=ide,index=1" % scratch,
        "-vga", "none", "-device", "virtio-vga,xres=800,yres=600", "-display", "none",
        "-serial", "unix:%s,server,nowait" % SOCK,
        "-qmp", "unix:%s,server,nowait" % QMP,
        "-no-reboot", "-no-shutdown",
        # ROOM TO GROW, which has to be asked for at start-up: a machine
        # without spare CPU slots and a maxmem above its memory cannot be
        # added to at all, whatever the guest supports.
        "-m", "1G,slots=4,maxmem=4G", "-smp", "2,maxcpus=4",
        "-accel", "tcg,thread=multi",
    ]
    log = open(os.path.join(BUILD, "hotplug-qemu.log"), "wb")
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
            raise SystemExit("hotplug: the guest never opened its serial socket")
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

        def report(tag):
            buf.clear()
            s.sendall(b"test hotplug\n")
            drain(10)
            text = buf.decode("utf-8", "replace")
            if "test hotplug: SKIP" in text:
                fails.append("%s: the guest found no hot-plug registers" % tag)
                return None
            m = re.findall(r"\[hotplug\] (\d+) cpu\(s\) online, (\d+) free page\(s\); "
                           r"added since boot: (\d+) cpu\(s\), (\d+) region\(s\)", text)
            if not m:
                fails.append("%s: the guest never answered `test hotplug`" % tag)
                return None
            cores, pages, cpus_added, mem_added = (int(x) for x in m[-1])
            print("hotplug: %-16s %d core(s), %d free page(s), +%d cpu +%d mem"
                  % (tag, cores, pages, cpus_added, mem_added))
            return cores, pages, cpus_added, mem_added

        drain(70)
        before = report("at boot")

        q = Qmp(QMP)
        # WHICH SLOT IS EMPTY is the machine's business, not ours: ask.
        slots = q.cmd("query-hotpluggable-cpus")["return"]
        empty = [c for c in slots if "qom-path" not in c]
        if not empty:
            fails.append("the machine has no free CPU slot to add one to")
        else:
            c = empty[-1]
            props = dict(c["props"])
            props["id"] = "newcpu"
            props["driver"] = c["type"]
            q.cmd("device_add", **props)
            print("hotplug: added a %s over QMP" % c["type"])

        q.cmd("object-add", **{"qom-type": "memory-backend-ram",
                               "id": "newmem", "size": 256 * 1024 * 1024})
        q.cmd("device_add", driver="pc-dimm", id="newdimm", memdev="newmem")
        print("hotplug: added a 256 MiB pc-dimm over QMP")

        time.sleep(6)
        after = report("after adding")

        if before and after:
            if after[0] <= before[0]:
                fails.append("A PROCESSOR WAS ADDED and the guest still has %d "
                             "core(s) online" % after[0])
            if after[2] < 1:
                fails.append("the guest did not count a CPU as added")
            if after[1] <= before[1]:
                fails.append("256 MiB was added and the allocator has %d free "
                             "page(s), down from %d -- the page bitmap was "
                             "never extended" % (after[1], before[1]))
            if after[3] < 1:
                fails.append("the guest did not count a memory region as added")
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    if fails:
        for m in fails:
            print("hotplug: FAIL -- %s" % m)
        return 1
    print("=== test-hotplug: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
