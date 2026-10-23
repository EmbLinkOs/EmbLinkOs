#!/usr/bin/env python3
"""power_shot.py -- can the machine be turned off from its own interface?

    python3 tools/power_shot.py

THE CLAIM. Until this, the only way to stop an EmbLink machine was to cut its
power: the kernel could always do it (power_transition, which `test power`
exercises) and nothing in userspace could ask. A daily driver needs a shutdown
that is not a wall socket.

HOW IT JUDGES, and it is the one test here that does not look at pixels: it
opens the EmbLink menu, clicks Shut Down, and waits for QEMU'S PROCESS TO EXIT.
Nothing about a screenshot can tell you the machine stopped -- a frozen picture
and a powered-off machine look identical -- but a guest that has handed itself
back to firmware takes its emulator with it.
"""
import os, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S

# The EmbLink menu title in the top bar, and Shut Down's position MEASURED off
# a screendump of the menu once open -- the first version guessed a row pitch
# and clicked below the menu entirely, which looks exactly like a shutdown that
# did not work.
MENU_TITLE = (80, 12)
SHUTDOWN   = (135, 138)


def main():
    if any(subprocess.run(["pgrep", "-x", e], capture_output=True).returncode == 0
           for e in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("power_shot: another qemu-system is running; refusing to start a second one")
        return 2

    p, ser, qmp = A.boot("power", os.path.join(A.BUILD, "shot-power.img"))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        log = open(os.path.join(A.BUILD, "power-serial.log"), "wb")

        def drain():
            while True:
                try:
                    b = s.recv(4096)
                except OSError:
                    return
                if not b:
                    return
                log.write(b); log.flush()
        threading.Thread(target=drain, daemon=True).start()

        t0 = time.time()
        while time.time() - t0 < 200:
            if b"first frame presented" in open(log.name, "rb").read():
                break
            time.sleep(1)
        time.sleep(8)

        S.move(q, *MENU_TITLE)
        S.click(q)
        time.sleep(2)
        q.screendump(os.path.join(A.BUILD, "power-menu.ppm"))
        print("power_shot: opened the EmbLink menu")

        S.move(q, *SHUTDOWN)
        time.sleep(0.6)
        q.screendump(os.path.join(A.BUILD, "power-aim.ppm"))
        S.click(q)
        print("power_shot: clicked Shut Down at %s" % (SHUTDOWN,))

        # THE MACHINE STOPPING IS NOT THE EMULATOR EXITING -- this harness runs
        # qemu with -no-shutdown, so a guest that powers itself off leaves the
        # process alive and PAUSED. Asking QMP for the run state is the honest
        # question; the kernel's own "flushed N dirty page(s) before power off"
        # on the serial line is the corroborating one, and it also proves the
        # shutdown was ORDERLY rather than a halt.
        state, flushed = "running", False
        for _ in range(40):
            try:
                r = q.cmd("query-status")
                state = (r.get("return") or {}).get("status", state)
            except Exception:
                pass
            seen = open(log.name, "rb").read()
            flushed = b"before power off" in seen or b"power: flushed" in seen
            if state != "running" or flushed:
                break
            time.sleep(1)

        print("power_shot: qemu run state %r; kernel reported an orderly flush: %s"
              % (state, "yes" if flushed else "no"))
        fails = []
        if state == "running" and not flushed:
            fails.append("the machine did not power off")
        elif not flushed:
            fails.append("the machine stopped but the kernel never flushed -- "
                         "that is a halt, not a shutdown")
        for f in fails:
            print("power_shot: FAIL %s" % f)
        print("power_shot: %s" % ("OK" if not fails else "FAILED"))
        return 1 if fails else 0
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
