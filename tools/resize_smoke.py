#!/usr/bin/env python3
"""Live resize: a window dragged by its corner grip is its new size while held.

Boots a disposable desktop, opens Settings, takes its corner grip and, without
letting go, asserts:

  out     dragged right and down (up to 200 by 100), it is already that much
          bigger -- not after the release -- and drawn at it: where the
          wallpaper was, beside the old right edge, is the window now
  in      dragged back in past where it began, it is that much smaller
  still   no zoom flew during the drag (a grip step is never a jump), and the
          kernel logged no resize per step
  done    let go, the app says its size once, the size the hand left it at

and photographs the window mid-drag. --arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect


def pixel(path, x, y):
    data = Path(path).read_bytes()
    # P6: magic, width height, maxval, then RGB
    head, rest = data.split(b'\n', 1)
    dims, rest = rest.split(b'\n', 1)
    _, rest = rest.split(b'\n', 1)
    w = int(dims.split()[0])
    o = (y * w + x) * 3
    return tuple(rest[o:o + 3])


def sized(d, want, seen, seconds=10.0):
    """Settings' frame once its size is `want` (w, h) within 2 px; ghosts seen on the way."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        wins, ghosts, _ = state(d)
        seen.extend(ghosts)
        f = frame_rect(wins['Settings'])
        if abs(f[2] - want[0]) <= 2 and abs(f[3] - want[1]) <= 2:
            return f
    raise AssertionError(f'Settings did not reach {want} while held: {f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-resize-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        P.move(d, 640, 20); d.g.pump(1)              # off the window: no hover in the way
        wins, _, _ = state(d)
        S = frame_rect(wins['Settings'])
        probe = (S[0] + S[2] + 30, S[1] + S[3] // 2)   # wallpaper now, window once it grows
        before = d.g.capture(out / 'before.ppm')

        # OUT: grab the grip, drag outward (as far as the screen lets the
        # pointer go, up to 200 by 100), keep holding
        n0 = len(d.g.seen)
        gx, gy = S[0] + S[2] - 10, S[1] + S[3] - 10
        ox, oy = min(200, P.W - 12 - gx), min(100, P.H - 12 - gy)
        assert ox >= 40 and oy >= 20, f'no room to grow Settings: {S}'
        P.move(d, gx, gy); d.g.pump(0.5)
        P.press(d, True); d.g.pump(0.5)
        P.glide(d, gx + ox, gy + oy)
        ghosts = []
        big = sized(d, (S[2] + ox, S[3] + oy), ghosts)
        d.g.pump(1)
        shot = d.g.capture(out / 'held.ppm')
        assert pixel(shot, *probe) != pixel(before, *probe), \
            f'the window grew to {big} but is not drawn there: {pixel(shot, *probe)} at {probe}'
        assert big[:2] == S[:2], f'the window moved as it grew: {big} from {S}'
        print(f'out: held, Settings is {big[2]}x{big[3]} from {S[2]}x{S[3]} and drawn at it ({shot})', flush=True)

        # IN: back past where it began, still held
        P.glide(d, gx - 150, gy - 80)
        small = sized(d, (S[2] - 150, S[3] - 80), ghosts)
        print(f'in: held, Settings is {small[2]}x{small[3]}', flush=True)

        # STILL: no zoom in the air, no resize logged per step
        assert not [g for g in ghosts if g['kind'] == 'zoom'], f'a grip step flew as a zoom: {ghosts}'
        steps = d.g.seen[n0:].count(b'compositor: win ')
        assert steps == 0, f'the kernel logged {steps} resizes during the drag'
        assert b'Settings: resized to' not in d.g.seen[n0:], 'the app announced a size before it was let go'
        print('still: no zoom, no resize logged while held', flush=True)

        # DONE: let go
        P.press(d, False)
        want = f'Settings: resized to {small[2]}x{small[3]}'.encode()
        d.g.wait(want, 20, start=n0); d.g.pump(2)
        assert d.g.seen[n0:].count(b'Settings: resized to') == 1, 'the app announced its size more than once'
        wins, _, _ = state(d)
        assert frame_rect(wins['Settings']) == small, f'Settings changed size after it was let go: {wins["Settings"]}'
        print(f'done: let go at {small[2]}x{small[3]}, said once', flush=True)
    finally:
        d.g.close()
    print('PASS: the corner grip resizes the window as it is dragged, both ways, with no zoom and one '
          'announcement at the end', flush=True)


if __name__ == '__main__':
    main()
