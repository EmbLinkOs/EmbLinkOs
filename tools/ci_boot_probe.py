#!/usr/bin/env python3
"""Boot the x86 image once per configuration and say how far it got.

    EMBLINK_OVMF_DIR=... python3 tools/ci_boot_probe.py tcg:max kvm:host kvm:max kvm:qemu64

Each argument is ACCEL:CPU (QEMU's -accel and -cpu). A boot that reaches the
first-boot Setup's first frame is a desktop; one that prints the kernel's
exception banner first is a crash, and its last serial lines are shown. CI runs
this before the smokes, so a machine the kernel cannot run on fails in minutes
with the reason, not in two hours of timeouts.
"""
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from recovery_smoke import Guest   # noqa: E402

UP = b'EmbLink OS Setup: first frame presented'
DOWN = b'=== Exception ==='


def probe(accel, cpu, seconds):
    os.environ['EMBLINK_ACCEL'] = accel
    os.environ['EMBLINK_CPU'] = cpu
    out = Path(tempfile.mkdtemp(prefix='emblink-probe-'))
    disk = out / 'disk.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    g = Guest(out / 'guest', disk, boot_nvme=True, resolution=(1280, 800))
    t0 = time.monotonic()
    try:
        while time.monotonic() - t0 < seconds:
            g.pump(1)
            if UP in g.seen:
                return True, f'desktop in {time.monotonic() - t0:.0f} s', b''
            if DOWN in g.seen:
                time.sleep(1); g.pump(1)
                return False, 'kernel exception', bytes(g.seen[g.seen.rfind(DOWN) - 1500:])
        return False, f'nothing in {seconds} s', bytes(g.seen[-1500:])
    finally:
        g.close()


def main():
    seconds = int(os.environ.get('EMBLINK_PROBE_SECONDS', '240'))
    picked = None
    for spec in sys.argv[1:] or ['kvm:max']:
        accel, _, cpu = spec.partition(':')
        ok, what, tail = probe(accel, cpu or 'max', seconds)
        print(f'{spec:24} {"OK  " if ok else "FAIL"} {what}', flush=True)
        if tail:
            print(tail.decode('utf-8', 'replace'), flush=True)
        if ok and not picked:
            picked = (accel, cpu or 'max')
    # the first that boots is what the smokes run on (GitHub Actions: $GITHUB_ENV)
    if picked and os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a') as f:
            f.write(f'EMBLINK_ACCEL={picked[0]}\nEMBLINK_CPU={picked[1]}\n')
    if picked:
        print(f'picked: -accel {picked[0]} -cpu {picked[1]}', flush=True)
    sys.exit(0 if picked else 1)


if __name__ == '__main__':
    main()
