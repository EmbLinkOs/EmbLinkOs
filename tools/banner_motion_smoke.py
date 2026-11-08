#!/usr/bin/env python3
"""Notification banners in motion, watched in captures.

Boots a disposable desktop, posts system notices over the serial console
(`perf notice TITLE`, which the banner service mirrors as any kernel notice)
and photographs the top-right corner against itself with no banner up. The
banner window's place comes from the compositor (`perf windows`). It asserts:

  in       a banner slides in from the screen's edge: some capture finds its
           left edge between the edge and where it comes to rest
  stack    a second notice goes on top and the first glides down to make room:
           some capture finds the first between its old place and its new one
  out      left alone, a banner's time runs out and it slides back out the
           way it came, and the window goes with the last one
  back     dragged a little and let go slowly, a banner springs home -- and
           while the pointer is on it, its time does not run out
  flick    dragged well to the right and let go, it is thrown off the screen,
           and the one below glides up into its place

Each starts from an empty corner with fresh notices: a banner's six seconds
are shorter than an emulator's patience.

Banners are found by the stripe down each one's left side (the level's
colour, saturated where the glass, its text, shadow and the wallpaper are
not): along a row for a banner's left edge, down the stripes' column for the
stack. x86 only: arm64 has no sessions, so no system notices to mirror.
"""
import argparse
import tempfile
import time
from pathlib import Path

from PIL import Image

import perf_smoke as P
from motion_smoke import state
from dock_motion_smoke import aim

DIFF = 18
SAT = 50                        # the stripe's saturation; nothing else here has it
PANEL_W, EDGE = 340, 22         # user/system/notifyd/notifyd.c


def shot(d, out, name):
    return Image.open(d.g.capture(out / f'{name}.ppm')).convert('RGB')


def stripe(a, b, x, y):
    """A banner's level stripe at (x, y): changed from the empty corner, and saturated."""
    p = a.getpixel((x, y))
    return max(p) - min(p) > SAT and max(abs(u - v) for u, v in zip(p, b.getpixel((x, y)))) > DIFF


def left_edge(img, empty, row, x0, x1):
    """A banner's left edge on `row` in [x0, x1), or None."""
    return next((x for x in range(x0, x1) if stripe(img, empty, x, row)), None)


def runs(img, empty, col, y0, y1):
    """The banners down column `col` (the stripes'): (top, bottom) of each."""
    out, start = [], None
    for y in range(y0, y1):
        c = stripe(img, empty, col, y)
        if c and start is None:
            start = y
        elif not c and start is not None:
            if y - start > 20:
                out.append((start, y))
            start = None
    if start is not None and y1 - start > 20:
        out.append((start, y1))
    return out


def watch(d, out, until, seconds=4.0):
    """Captures until `until(img)` says so; all of them (one file reused)."""
    seen, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        img = shot(d, out, 'sample')
        seen.append(img)
        if until(img):
            break
    return seen


def banner_window(d, screen_w, seconds=20.0):
    """The banner window, once it is in its corner: its right edge on the
    screen's (it is placed by its first frame, after it is created)."""
    end, w = time.monotonic() + seconds, None
    while time.monotonic() < end:
        w = state(d)[0].get('Notifications')
        if w and w['x'] + w['w'] == screen_w:
            return w
    raise AssertionError(f'no banner window came up in the corner: {w}')


def gone(d, seconds=20.0):
    """Wait for the banner window to have gone (every banner out)."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if 'Notifications' not in state(d)[0]:
            return
    raise AssertionError('the banner window outlived its banners')


def at_rest(d, out, empty, col, win, count, seconds=8.0):
    """Wait for `count` banners down the stripes' column, unmoved for half a
    second (two captures in a row can be one composed frame); returns them."""
    last, since, end = None, 0.0, time.monotonic() + seconds
    while time.monotonic() < end:
        r = runs(shot(d, out, 'sample'), empty, col, win['y'], win['y'] + win['h'])
        now = time.monotonic()
        if r != last:
            last, since = r, now
        elif len(r) == count and now - since >= 0.5:
            return r
    raise AssertionError(f'expected {count} banner(s) at rest, saw {last}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64',), default='x86_64')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-banner-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 400, 500); d.g.pump(3)
        empty = shot(d, out, 'empty')
        W = empty.size[0]
        rest_x = W - EDGE - PANEL_W                  # a banner's left edge at rest
        col = rest_x + 2                             # down the stripes

        # IN: from the screen's edge
        d.g.command('perf notice First', b'perf: notice', 60)
        win = banner_window(d, W)
        row = win['y'] + 10 + 30                     # through the top banner
        seen = watch(d, out, lambda im: left_edge(im, empty, row, win['x'], W) == rest_x)
        edges = [left_edge(im, empty, row, win['x'], W) for im in seen]
        assert edges[-1] == rest_x, f'the banner did not come to rest at x {rest_x}: {edges[-6:]}'
        mid = [e for e in edges if e is not None and rest_x + 16 < e < W - EDGE - 20]
        assert mid, f'the banner appeared in place, it did not slide in: {edges}'
        seen[edges.index(mid[0])].save(out / 'in-mid.png')
        print(f'in: slid in from the edge, caught at x {mid[0]} on its way to {rest_x} '
              f'({len(seen)} captures)', flush=True)
        first = runs(seen[-1], empty, col, win['y'], win['y'] + win['h'])
        assert len(first) == 1, f'expected one banner down the column, found {first}'

        # STACK: a second goes on top; the first glides down
        d.g.command('perf notice Second', b'perf: notice', 60)
        top0 = first[0][0]
        def two(im):
            r = runs(im, empty, col, win['y'], win['y'] + win['h'])
            return len(r) == 2 and r[1][0] > top0 + 40
        seen = watch(d, out, two)
        tops = [(runs(im, empty, col, win['y'], win['y'] + win['h']) or [(None,)])[-1][0] for im in seen]
        final = runs(seen[-1], empty, col, win['y'], win['y'] + win['h'])
        assert len(final) == 2, f'two notices are not two banners down the column: {final}'
        glide = [y for y in tops if y is not None and top0 + 4 < y < final[1][0] - 4]
        assert glide, f'the first banner jumped down rather than gliding: {tops}'
        seen[tops.index(glide[0])].save(out / 'stack-mid.png')
        print(f'stack: the first glided down from y {top0} to {final[1][0]}, caught at {glide[0]}', flush=True)
        upper, lower = final

        # OUT: their time runs out; each slides back out, and the window goes
        row2 = (lower[0] + lower[1]) // 2
        seen = watch(d, out, lambda im: left_edge(im, empty, row2, win['x'], W) is None, seconds=15)
        edges = [left_edge(im, empty, row2, win['x'], W) for im in seen]
        assert edges[-1] is None, f'the banner did not go: {edges[-6:]}'
        going = [e for e in edges if e is not None and rest_x < e < W - EDGE - 20]
        assert going, f'the banner vanished in place, it did not slide out: {edges[-12:]}'
        gone(d)
        print(f'out: slid out when its time was up (caught at x {going[0]}), and the window went', flush=True)

        # BACK: a short, slow drag springs home; on it, the time stands still
        d.g.command('perf notice Back', b'perf: notice', 60)
        win = banner_window(d, W)
        top = at_rest(d, out, empty, col, win, 1)[0]
        y_hold = (top[0] + top[1]) // 2
        P.move(d, rest_x + 60, y_hold); d.g.pump(0.5)
        P.press(d, True)
        for _ in range(4):
            aim(d, P.S._pos[0] + 10, y_hold); time.sleep(0.15)
        d.g.pump(0.8)                                # held still: no throw left in it
        P.press(d, False)
        d.g.pump(1.5)
        edge = left_edge(shot(d, out, 'back'), empty, y_hold, win['x'], W)
        assert edge == rest_x, f'a short drag let go slowly did not spring home: edge {edge}'
        print('back: dragged 40 px and let go slowly, it sprang home', flush=True)
        d.g.pump(7)                                  # past its time, with the pointer on it
        edge = left_edge(shot(d, out, 'kept'), empty, y_hold, win['x'], W)
        assert edge == rest_x, f'with the pointer on it, the banner still went: edge {edge}'
        print('back: with the pointer on it, it outlived its six seconds', flush=True)
        P.move(d, 400, 500)
        gone(d)

        # FLICK: thrown off; the one below glides up
        d.g.command('perf notice Below', b'perf: notice', 60)
        d.g.command('perf notice Above', b'perf: notice', 60)
        win = banner_window(d, W)
        upper, lower = at_rest(d, out, empty, col, win, 2)
        y_hold = (upper[0] + upper[1]) // 2
        P.move(d, rest_x + 60, y_hold); d.g.pump(0.3)
        P.press(d, True)
        for _ in range(5):
            aim(d, P.S._pos[0] + 40, y_hold)
        P.press(d, False)
        def thrown(im):
            r = runs(im, empty, col, win['y'], win['y'] + win['h'])
            return len(r) == 1 and abs(r[0][0] - upper[0]) <= 3
        seen = watch(d, out, thrown, seconds=5)
        left = runs(seen[-1], empty, col, win['y'], win['y'] + win['h'])
        assert thrown(seen[-1]), f'after the flick the other banner is not on top where the first was: {left}'
        print(f'flick: thrown off; the one below glided up to y {left[0][0]} ({len(seen)} captures)', flush=True)
    finally:
        d.g.close()
    print('PASS: banners slide in, stack and glide, slide out when their time is up (not while '
          'the pointer is on them), spring home from a short drag and fly off when flicked', flush=True)


if __name__ == '__main__':
    main()
