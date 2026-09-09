#!/usr/bin/env python3
"""Drive keyboard and pointer events into a running aarch64 guest over QMP.

    usage: arm64_input_probe.py <qmp-port> [seconds]

WHY THE ACCEPTANCE TEST NEEDS THIS. Every other check in test-arm64-boot is a
line the kernel prints about itself. Input is the one thing the machine cannot
test alone: virtio-input can enumerate both devices, arm its queue and report
"polled" while delivering nothing at all, and the difference is invisible until
someone presses a key. So the test presses the keys.

It injects for the WHOLE run rather than once, because the kernel's input window
is a few seconds somewhere inside the boot and this script has no way to know
when -- the first attempt at this sent four keys at a fixed moment, missed the
window entirely, and reported zero events against a driver that was working.

Failures here are deliberately NOT fatal: QMP may not be up yet, and a run
without a monitor is still a valid run of everything else.
"""

import json
import socket
import sys
import time


def main(argv):
    port = int(argv[1]) if len(argv) > 1 else 4444
    secs = float(argv[2]) if len(argv) > 2 else 20.0

    # Wait for the monitor to accept, but not forever.
    deadline = time.time() + secs
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            break
        except OSError:
            time.sleep(0.2)
    if sock is None:
        print("arm64_input_probe: QMP never accepted; skipping input injection")
        return 0

    f = sock.makefile("rw")
    f.readline()                                   # the greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()

    def cmd(name, args=None):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        f.write(json.dumps(msg) + "\n")
        f.flush()
        while True:
            line = f.readline()
            if not line:
                return None
            m = json.loads(line)
            if "return" in m or "error" in m:      # skip asynchronous events
                return m

    # The tablet's range is 0..32767 whatever the screen is; the driver reads
    # the real maximum out of the device config and scales, so these are just
    # "somewhere sensible and moving".
    i = 0
    while time.time() < deadline:
        try:
            cmd("send-key", {"keys": [{"type": "qcode", "data": "a"}]})
            cmd("input-send-event", {"events": [
                {"type": "abs", "data": {"axis": "x", "value": (i * 500) % 32767}},
                {"type": "abs", "data": {"axis": "y", "value": (i * 700) % 32767}},
            ]})
        except OSError:
            return 0                               # guest exited: fine
        i += 1
        time.sleep(0.3)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
