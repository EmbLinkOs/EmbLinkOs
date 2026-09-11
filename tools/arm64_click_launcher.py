#!/usr/bin/env python3
"""Press the top bar's launcher button in a running aarch64 guest, over QMP.

    usage: arm64_click_launcher.py <qmp-port> <serial-log>

WHY THE BOOT TEST PRESSES A BUTTON. The launcher button did nothing on aarch64
for as long as the port has had a desktop: /run was never mounted, so the top
bar's IPC request to the desktop went to the root disk and vanished. Every line
the boot test checked was still true -- the desktop came up, the compositor
presented, input events reached the driver -- because none of them is "a person
clicked something and it did what it says". This is.

It waits for the top bar's first frame, reads the screen size from the log (the
button is a fixed distance from the top-left corner at any resolution), moves
the tablet there, presses and releases, and waits for the desktop to log that
the launcher opened. The boot test then checks for that line. Nothing here
decides pass or fail; it only acts.
"""
import json, re, socket, sys, time

# The launcher mark: 8 px of padding, a 17 px icon, in a bar whose controls are
# vertically centred -- so its middle is about (16, 12) in screen pixels.
MARK_X, MARK_Y = 16, 12


def qmp(port, cmd, args=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    f = s.makefile("rw")
    f.readline()
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    f.write(json.dumps(msg) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            s.close()
            return r


def wait_for(log, needle, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            if needle in open(log, "rb").read():
                return True
        except OSError:
            pass
        time.sleep(0.5)
    return False


def main(argv):
    port, log = int(argv[1]), argv[2]
    if not wait_for(log, b"first frame presented", 60):
        return 1
    time.sleep(2)                       # let the desktop's listener come up
    m = re.search(rb"framebuffer (\d+)x(\d+)", open(log, "rb").read())
    w, h = (int(m.group(1)), int(m.group(2))) if m else (1280, 800)
    ax, ay = MARK_X * 32767 // w, MARK_Y * 32767 // h
    try:
        qmp(port, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}}]})
        time.sleep(0.5)
        qmp(port, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]})
        time.sleep(0.15)
        qmp(port, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]})
    except OSError:
        return 1
    return 0 if wait_for(log, b"home: launcher OPEN", 15) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
