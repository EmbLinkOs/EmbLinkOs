#!/usr/bin/env python3
"""notify_shot.py -- does a banner appear, from a program that has already exited?

    python3 tools/notify_shot.py

A notification is for saying something when your window is NOT what the user is
looking at, so the witness is a program that posts one and leaves. If a banner
is on screen after that process is gone, the notification service owns it --
which is the whole claim. Judged on pixels in the top-right corner, where the
banners live, so a change anywhere else on screen cannot pass for one.
"""
import os, subprocess, sys, threading, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_shot as A
import shell_shot as S


def main():
    if any(subprocess.run(["pgrep", "-x", exe], capture_output=True).returncode == 0
           for exe in ("qemu-system-x86_64", "qemu-system-aarch64")):
        print("notify_shot: another qemu-system is running; refusing to start a second one")
        return 2
    name = "notify"
    p, ser, qmp = A.boot(name, os.path.join(A.BUILD, "shot-%s.img" % name))
    try:
        s = A.connect(ser)
        q = A.Qmp(qmp)
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
        while time.time() - t0 < 180 and b"desktop ready" not in log["buf"]:
            time.sleep(1)
        time.sleep(8)
        served = b"notifyd: serving" in log["buf"]
        print("notify_shot: the service published its endpoint: %s" % ("yes" if served else "NO"))

        before = os.path.join(A.BUILD, "notify-before.ppm")
        q.screendump(before)

        s.sendall(b"run /data/apps/notifytest/notifytest.elf\n")
        time.sleep(6)
        after = os.path.join(A.BUILD, "notify-after.ppm")
        q.screendump(after)

        posted = b"embk_notify -> 0" in log["buf"]
        print("notify_shot: the poster reported success: %s" % ("yes" if posted else "NO"))

        # THE TOP-RIGHT CORNER ONLY. A whole-screen diff would also pass on the
        # clock ticking over or the CPU readout changing, neither of which is a
        # banner.
        f = S.region_changed(before, after, 620, 30, 1010, 160)
        ok = f > 0.05
        print("notify_shot: the banner corner changed %.1f%% %s"
              % (f * 100.0, "OK" if ok else "-- NO BANNER"))
        return 0 if (ok and served and posted) else 1
    finally:
        p.kill()


if __name__ == "__main__":
    sys.exit(main())
