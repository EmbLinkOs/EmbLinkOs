#!/usr/bin/env python3
"""The tile preview: a window held against an edge shows where it will go.

Boots a disposable desktop, opens Settings and drags it by its bar. It
asserts, from `perf windows` (its `tile` line: the wash's rect, opacity and
target):

  left     held against the left edge of the work area, the wash grows toward
           its left half; let go, the window takes exactly that rect, and the
           wash fades
  right    the same against the right edge
  middle   held away from the edges, there is no wash

and photographs the wash while it is held. --arch aarch64 runs it on arm64
under HVF; --graphics virgl composes on the host's GPU (Linux).
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect


def tile(d):
    """The wash: (on, rect, alpha, target), or None."""
    text = d.g.command('perf windows', b'perf: end', 60)
    for line in text.splitlines():
        if line.startswith('perf: tile '):
            kv = dict(p.split('=', 1) for p in line[11:].split() if '=' in p)
            return (int(kv['on']), [int(kv[k]) for k in ('x', 'y', 'w', 'h')], int(kv['alpha']),
                    [int(v) for v in kv['to'].split(',')])
    return None


def held_at(d, want_on, seconds=10.0):
    """Poll until the wash is (or is not) on and has stopped growing."""
    last, end = None, time.monotonic() + seconds
    while time.monotonic() < end:
        t = tile(d)
        if want_on and t and t[0] == 1 and t == last and t[1] == t[3]:
            return t
        if not want_on and (t is None or t[0] == 0):
            return t
        last = t
    raise AssertionError(f'the wash did not {"settle" if want_on else "go"}: {last}')


def drag_to(d, x, y, fx, fy):
    """Press at (x, y), sweep to (fx, fy), keep holding."""
    P.move(d, x, y); d.g.pump(0.3)
    P.press(d, True)
    P.glide(d, fx, fy, step=24, pause=0.02)
    d.g.pump(0.5)


def landed(d, rect, seconds=15.0):
    end, win = time.monotonic() + seconds, None
    while time.monotonic() < end:
        wins, ghosts, _ = state(d)
        win = wins.get('Settings')
        if win and not ghosts and frame_rect(win) == rect:
            return win
    raise AssertionError(f'Settings did not land on {rect}: {win}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-tile-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        d.g.command('perf effects full', b'perf: effects', 60)   # not what is tested
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        win = state(d)[0]['Settings']
        dz = win['drag0']
        grab = (win['x'] + dz[0] + 40, win['y'] + dz[1] + dz[3] // 2)

        # MIDDLE: a drag away from the edges shows nothing. (Not under the Lima
        # VM's nested emulation: it stalls and then takes queued input in a
        # burst, so this press and the next arrive back to back -- a double
        # click, which zooms the window.)
        if args.graphics != 'virgl':
            drag_to(d, *grab, grab[0] + 60, grab[1] + 40)
            t = tile(d)
            assert t is None or t[0] == 0, f'a wash away from the edges: {t}'
            P.press(d, False); d.g.pump(1)
            print('middle: no wash away from the edges', flush=True)

        # LEFT
        win = state(d)[0]['Settings']
        grab = (win['x'] + dz[0] + 40, win['y'] + dz[1] + dz[3] // 2)
        drag_to(d, *grab, 30, grab[1])
        on, rect, alpha, target = held_at(d, True)
        assert target[0] == 0 and target[2] == 640 and target[1] >= 30 and target[3] > 500, \
            f'held against the left edge, the wash is not the left half: {target}'
        shot = d.g.capture(out / 'tile-left.ppm')
        P.press(d, False)
        landed(d, target)
        held_at(d, False)
        print(f'left: the wash showed {target}, and the window took it ({shot})', flush=True)

        # RIGHT
        win = state(d)[0]['Settings']
        grab = (win['x'] + dz[0] + 40, win['y'] + dz[1] + dz[3] // 2)
        drag_to(d, *grab, 1270, grab[1])
        on, rect, alpha, target2 = held_at(d, True)
        assert target2 == [640, target[1], 640, target[3]], \
            f'held against the right edge, the wash is not the right half: {target2}'
        P.press(d, False)
        landed(d, target2)
        print(f'right: the wash showed {target2}, and the window took it', flush=True)
    finally:
        d.g.close()
    print('PASS: a window held against an edge shows where it will tile, takes exactly that '
          'rect when let go, and away from the edges shows nothing', flush=True)


if __name__ == '__main__':
    main()
