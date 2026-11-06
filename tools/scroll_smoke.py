#!/usr/bin/env python3
"""Scrolling moves the pixels it has: and leaves them exactly right.

Boots a disposable desktop, opens Settings and photographs its pane, then
turns the wheel over the pane and asserts:

  down    eight notches down glide the page: the pane is no longer what it
          was, and the photograph mid-glide is kept
  back    eight notches up bring it to the top again, and once it is still the
          pane is the SAME picture, pixel for pixel, as before it moved

Every frame in between that moved the page was a scroll blit (rows copied, a
strip painted), so a blit that left a stale pixel, copied by a rounded amount
or smeared a clip's edge would show in `back`. --arch aarch64 runs it on arm64
under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect


def region(path, box):
    """The RGB bytes of `box` (x, y, w, h) in a P6 capture."""
    data = Path(path).read_bytes()
    head, rest = data.split(b'\n', 1)
    dims, rest = rest.split(b'\n', 1)
    _, rest = rest.split(b'\n', 1)
    w = int(dims.split()[0])
    x, y, bw, bh = box
    return b''.join(rest[((y + r) * w + x) * 3:((y + r) * w + x + bw) * 3] for r in range(bh))


def wheel(d, down, n):
    for _ in range(n):
        b = 'wheel-down' if down else 'wheel-up'
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': b}}])
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': b}}])
        time.sleep(0.12)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-scroll-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        wins, _, _ = state(d)
        S = frame_rect(wins['Settings'])
        # the pane: right of the sidebar, under the bar, clear of the frame
        pane = (S[0] + 260, S[1] + 70, S[2] - 300, S[3] - 110)
        P.move(d, pane[0] + pane[2] // 2, pane[1] + pane[3] // 2); d.g.pump(2)
        before = d.g.capture(out / 'before.ppm')
        top = region(before, pane)

        wheel(d, True, 8)
        mid = d.g.capture(out / 'scrolled.ppm')
        d.g.pump(2)
        moved = region(d.g.capture(out / 'down.ppm'), pane)
        assert moved != top, 'eight notches down did not move the pane'
        print(f'down: eight notches moved the page ({mid})', flush=True)

        wheel(d, False, 8)
        d.g.pump(3)
        after = d.g.capture(out / 'after.ppm')
        back = region(after, pane)
        if back != top:
            diff = sum(1 for a, b in zip(back, top) if a != b)
            raise AssertionError(f'back at the top the pane differs in {diff} bytes ({before} vs {after})')
        print('back: at the top again the pane is the same picture, pixel for pixel', flush=True)
        print('PASS: a scroll moves the pixels it has, and leaves every one of them right')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
