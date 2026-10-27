#!/usr/bin/env python3
"""The overview (GUI+Up): every window on the space at once, side by side.

Boots a disposable desktop, opens Settings and the Terminal, and asserts:

  in       GUI+Up flies both windows (`overview` ghosts) to places of their
           own: inside the work area, apart, each its own shape and no larger
           than itself
  pick     a click on the window that was behind brings it in front as both
           fly home -- each lands exactly where it was
  out      GUI+Up again and Esc sends them home as they were: the one in
           front stays in front
  still    an overview at rest composes (nearly) nothing: windows held in
           their places are not motion

and photographs the overview. --arch aarch64 runs it on arm64 under HVF;
--graphics virgl composes on the host's GPU (Linux, as tools/virgl_smoke.py).
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect


def settled(d, kind, count, seconds=15.0):
    """Ghosts of `kind`, `count` of them, unmoved across two samples."""
    last, end = None, time.monotonic() + seconds
    while time.monotonic() < end:
        _, ghosts, _ = state(d)
        mine = sorted([(g['app'], g['x'], g['y'], g['w'], g['h']) for g in ghosts if g['kind'] == kind])
        if len(mine) == count and mine == last:
            return {g[0]: g[1:] for g in mine}
        last = mine
    raise AssertionError(f'{count} {kind} ghosts did not settle: {last}')


def home(d, seconds=15.0):
    """No ghost in the air; the windows."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        wins, ghosts, _ = state(d)
        if not ghosts and 'Settings' in wins and 'Terminal' in wins:
            return wins
    raise AssertionError(f'the windows did not come home: {ghosts}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-overview-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        # the effects tier held: under TCG the overview's own entry is heavy
        # enough to step it, and a step repaints everything (not what is tested)
        d.g.command('perf effects full', b'perf: effects', 60)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        n0 = len(d.g.seen)
        P.click(d, P.DOCK_TERMINAL_X, P.DOCK_Y)
        d.g.wait(b'Terminal: first frame presented', start=n0); d.g.pump(3)
        before = home(d)
        assert before['Terminal']['z'] > before['Settings']['z'], 'the Terminal did not open in front'

        # IN
        n0 = len(d.g.seen)
        d.key('meta_l-up')
        d.g.wait(b'compositor: overview of 2 windows', 20, start=n0)
        places = settled(d, 'overview', 2)
        W, H = 1280, 800
        for app, (x, y, w, h) in places.items():
            title = 'Settings' if app == 'settings.elf' else 'Terminal'
            fw, fh = before[title]['w'], before[title]['h']
            assert 0 <= x and x + w <= W and 34 <= y and y + h <= H - 70, f'{app} is not in the work area: {(x, y, w, h)}'
            assert w <= fw and h <= fh, f'{app} grew in the overview: {(w, h)} from {(fw, fh)}'
            assert abs(w * fh - h * fw) <= max(fw, fh) * 2, f'{app} lost its shape: {(w, h)} from {(fw, fh)}'
        (a, ra), (b, rb) = places.items()
        apart = ra[0] + ra[2] <= rb[0] or rb[0] + rb[2] <= ra[0] or ra[1] + ra[3] <= rb[1] or rb[1] + rb[3] <= ra[1]
        assert apart, f'the windows overlap in the overview: {places}'
        shot = d.g.capture(out / 'overview.ppm')
        print(f'in: both windows apart in the work area ({shot})', flush=True)
        # STILL: an overview at rest is not motion -- it composes (nearly) nothing
        d.g.command('perf reset', b'perf: reset', 60); d.g.pump(2)
        composes = P.perf(d).get('composes', 0)
        assert composes <= 4, f'an overview at rest composed {composes} times in two seconds'
        print(f'still: {composes} composes in two seconds of a still overview', flush=True)

        # KEYS: a click on empty desktop is the overview's -- it closes, nothing else
        # PICK: Settings, which was behind, comes in front
        sx, sy, sw, sh = places['settings.elf']
        n0 = len(d.g.seen)
        P.click(d, sx + sw // 2, sy + sh // 2)
        d.g.wait(b'overview closed, in front: settings.elf', 20, start=n0)
        after = home(d)
        for title in ('Settings', 'Terminal'):
            assert frame_rect(after[title]) == frame_rect(before[title]), \
                f'{title} did not land where it was: {after[title]} vs {before[title]}'
        assert after['Settings']['z'] > after['Terminal']['z'], 'the window picked is not in front'
        print('pick: Settings picked, in front, both home exactly', flush=True)

        # OUT: GUI+Up, then Esc -- home as they were
        n0 = len(d.g.seen)
        d.key('meta_l-up')
        d.g.wait(b'compositor: overview of 2 windows', 20, start=n0)
        settled(d, 'overview', 2)
        d.key('esc')
        d.g.wait(b'compositor: overview closed', 20, start=n0)
        again = home(d)
        assert again['Settings']['z'] > again['Terminal']['z'], 'Esc changed which window is in front'
        assert b'overview closed, in front' not in d.g.seen[n0:], 'Esc picked a window'
        print('out: Esc sent them home as they were', flush=True)
    finally:
        d.g.close()
    print('PASS: the overview lays the windows out apart, a click brings one in front as they fly '
          'home, and Esc leaves them as they were', flush=True)


if __name__ == '__main__':
    main()
