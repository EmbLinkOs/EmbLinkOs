#!/usr/bin/env python3
"""tools/ntp_test.py -- did the guest adopt the RIGHT time, not just A time?

The in-guest `test ntp` breaks the clock by four hundred days, syncs, and
measures what is left against the CMOS chip. That proves the sync moved the
clock back to where the machine started -- but the machine's own CMOS is not
an independent witness, and an SNTP client with the epoch constant wrong by
seventy years would still show a zero residual if the same constant were
applied both ways.

So this brackets the run with the HOST's clock and checks that the epoch the
guest adopted falls inside it. The host is the reference; nothing the kernel
computed takes part in it.
"""
import os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLACK = 10           # seconds: round trip, CMOS resolution, and the clock's own second

def main():
    env = dict(os.environ, NET="1")
    before = time.time()
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "console_test.py"),
                        "test ntp"],
                       cwd=ROOT, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    after = time.time()
    out = p.stdout
    sys.stdout.write(out)

    m = re.search(r"\[ntp\] server (\S+)\s+adopted (\d+)\s+step (-?\d+) s\s+residual (-?\d+) s", out)
    if not m:
        print("=== test-ntp: FAIL -- the guest never reported an adopted time")
        return 1
    server, adopted, step, residual = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))

    lo, hi = before - SLACK, after + SLACK
    print("=== host clock %d..%d, guest adopted %d from %s (step %+d s, residual %d s)"
          % (lo, hi, adopted, server, step, residual))
    if not (lo <= adopted <= hi):
        off = adopted - (before + after) / 2
        print("=== test-ntp: FAIL -- the adopted time is %+.0f s from the host's; "
              "a 2208988800 s error means the NTP epoch, 0 means no sync" % off)
        return 1
    if p.returncode != 0:
        print("=== test-ntp: FAIL -- the guest's own check failed")
        return 1
    print("=== test-ntp: OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
