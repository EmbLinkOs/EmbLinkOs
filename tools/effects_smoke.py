#!/usr/bin/env python3
"""The effects governor: full material while frames are cheap, reduced while
they run long, and held when the person says so.

Boots a disposable desktop under TCG -- where dragging a translucent window,
which blurs again what is behind it at every step, costs a frame several
times over -- opens Settings and asserts:

  idle     a still desktop draws full effects
  steps    dragging Settings by its bar steps the effects down, by what the
           frames cost (the compositor says so), and once the hand is still
           they come back up
  held     `perf effects reduced` draws the reduced tier whatever frames cost
           (captured: Settings over a flat tint, no blur behind it), and
           `perf effects full` keeps full effects through the same drag

`perf` reports the tier (effects=0 full, 1 reduced), how it is chosen
(effects_mode) and how often it stepped (effects_steps). x86 under TCG only:
on a real core (arm64 under HVF) the same drags never need the reduced tier,
which is the point -- tools/motion_smoke.py there sees no step at all.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state


def fx(d):
    """(tier, mode, steps) from `perf`."""
    text = d.g.command('perf', b'perf: end', 60)
    line = next(l for l in text.splitlines() if l.startswith('perf: span_ms='))
    kv = dict(p.split('=', 1) for p in line[6:].split() if '=' in p)
    return int(kv['effects']), int(kv['effects_mode']), int(kv['effects_steps'])


def until_full(d, seconds=60):
    """Nudge the pointer now and then -- the governor judges frames, and a
    still desktop draws none -- until the effects are full again, which a
    run of dear frames can hold off for up to 16 s. Returns (tier, mode)."""
    end, tier, mode = time.monotonic() + seconds, 1, 0
    while time.monotonic() < end and tier != 0:
        P.move(d, 640 + (int(time.monotonic()) % 2) * 20, 760); d.g.pump(1)
        tier, mode, _ = fx(d)
    return tier, mode


def drag(d, x, y, seconds):
    """Hold the bar at (x, y) and sweep the window back and forth."""
    P.move(d, x, y); d.g.pump(0.3)
    P.press(d, True)
    end, k = time.monotonic() + seconds, 0
    while time.monotonic() < end:
        P.glide(d, x + (120 if k % 2 == 0 else 0), y + (60 if k % 2 == 0 else 0), step=12, pause=0.02)
        k += 1
    P.glide(d, x, y, step=12, pause=0.02)
    P.press(d, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64',), default='x86_64')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-effects-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(4)
        win = state(d)[0]['Settings']
        dz = win['drag0']
        bar = (win['x'] + dz[0] + dz[2] // 2, win['y'] + dz[1] + dz[3] // 2)

        # IDLE: once the launches are over, full effects
        tier, mode = until_full(d)
        assert tier == 0 and mode == 0, f'a still desktop is not drawing full effects: tier {tier} mode {mode}'
        print('idle: full effects', flush=True)

        # STEPS: down while dragged, up again once still
        n0 = len(d.g.seen)
        drag(d, *bar, seconds=4)
        d.g.wait(b'compositor: effects reduced (by cost', 30, start=n0)
        print('steps: dragging a translucent window stepped the effects down by cost', flush=True)
        until_full(d)
        d.g.wait(b'compositor: effects full (by cost', 5, start=n0)
        tier, mode, steps = fx(d)
        assert tier == 0 and steps >= 2, f'still again, the effects did not come back: tier {tier} steps {steps}'
        print(f'steps: still again, full effects came back ({steps} steps)', flush=True)

        # HELD: reduced whatever frames cost, then full through the drag
        d.g.command('perf effects reduced', b'perf: effects', 60); d.g.pump(2)
        tier, mode, _ = fx(d)
        assert tier == 1 and mode == 2, f'held reduced, the tier is {tier} (mode {mode})'
        shot = d.g.capture(out / 'reduced.ppm')
        print(f'held: reduced ({shot})', flush=True)
        d.g.command('perf effects full', b'perf: effects', 60); d.g.pump(2)
        n0 = len(d.g.seen)
        drag(d, *bar, seconds=3)
        tier, mode, _ = fx(d)
        assert tier == 0 and mode == 1 and b'effects reduced' not in d.g.seen[n0:], \
            f'held full, a drag still stepped the effects down: tier {tier} mode {mode}'
        shot = d.g.capture(out / 'full.ppm')
        print(f'held: full through a drag ({shot})', flush=True)
        d.g.command('perf effects auto', b'perf: effects', 60)
    finally:
        d.g.close()
    print('PASS: full effects while frames are cheap, reduced by cost under a heavy drag and back, '
          'and held either way when asked', flush=True)


if __name__ == '__main__':
    main()
