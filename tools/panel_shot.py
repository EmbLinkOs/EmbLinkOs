#!/usr/bin/env python3
"""panel_shot.py -- does the system Open panel actually open?

    python3 tools/panel_shot.py

The file panel is a SERVICE: an app calls /run/emlink.files and a separate
program draws the window. Nothing about that is visible from inside the
calling app, and "the service published its endpoint" does not prove a user
can pick a file with it. So this drives it the way a person would -- boot,
launch the editor, click its Open button, and photograph what appears -- and
judges it on the pixels: a panel is a new window, so a large region of the
screen must change, and it must change BECAUSE of the click.
"""
import os, subprocess, sys, threading, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S


def main():
    if any(subprocess.run(["pgrep", "-x", exe], capture_output=True).returncode == 0
           for exe in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("panel_shot: another qemu-system is running; refusing to start a second one")
        return 2
    name = "panel"
    p, ser, qmp = A.boot(name, os.path.join(A.BUILD, "shot-%s.img" % name))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
        # wait for the desktop, then launch the editor from the shell
        # DRAIN THE SERIAL THE WHOLE TIME, in the background. The first version
        # stopped reading at "desktop ready" and then asked whether the panel
        # service had announced itself -- but that line arrives just AFTER the
        # desktop is ready, so it reported NO for a service that was running.
        log = {"buf": b""}
        def drain():
            while True:
                try:
                    c = s.recv(4096)
                except Exception:
                    return
                if not c:
                    return
                log["buf"] += c
        threading.Thread(target=drain, daemon=True).start()

        t0 = time.time()
        while time.time() - t0 < 180:
            if b"desktop ready" in log["buf"]:
                break
            time.sleep(1)
        time.sleep(8)
        served = b"filepanel: serving" in log["buf"]
        print("panel_shot: the service published its endpoint: %s" % ("yes" if served else "NO"))

        # `run <path>` -- the kernel console's launch command, the same one
        # app_shot.py uses. A bare path is not a command and does nothing.
        s.sendall(b"run /data/apps/edit/edit.elf\n")
        time.sleep(14)
        before = os.path.join(A.BUILD, "panel-before.ppm")
        q.screendump(before)

        # The editor's Open button: measured off panel-before.ppm.
        S.move(q, OPEN_BTN[0], OPEN_BTN[1])
        S.click(q)
        time.sleep(6)
        S.move(q, 512, 700)
        time.sleep(1.5)
        after = os.path.join(A.BUILD, "panel-after.ppm")
        q.screendump(after)

        f = A.changed_fraction(open(before, "rb").read(), open(after, "rb").read())
        ok = f > 0.05
        print("panel_shot: clicking Open changed %.1f%% of the screen %s"
              % (f * 100.0, "OK" if ok else "-- NO PANEL APPEARED"))
        return 0 if (ok and served) else 1
    finally:
        p.kill()


# The editor's Open button, MEASURED off panel-before.ppm rather than guessed
# (the first guess landed in the gap between Open and Save As, and reported
# "no panel appeared" for a click that hit nothing).
OPEN_BTN = (int(os.environ.get("OPEN_X", "592")), int(os.environ.get("OPEN_Y", "174")))

if __name__ == "__main__":
    sys.exit(main())
