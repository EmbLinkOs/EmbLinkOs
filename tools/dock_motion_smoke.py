#!/usr/bin/env python3
"""The dock's two motions, watched in captures.

Boots a disposable desktop and photographs the dock against itself at rest
(the pointer away from it, nothing launched). Each tile's rect comes from the
compositor (`perf windows` lists the tiles the desktop published). It asserts:

  plate   the pointer's plate sits under the tile the pointer is over (one
          plate: the icon button inside does not light a second), and moving
          to a tile two along it GLIDES there -- some capture on the way finds
          it between the two -- rather than jumping
  hop     clicking a tile whose app is not running makes its icon hop while
          the app starts: some capture finds the icon raised above where it
          rests, which is where it is again once the app's window is up

A tile is compared only with itself (tools/dock_truth.py says why). The plate
is read along one row just under the icons, inside the tiles: what differs
there from the resting dock is the plate. The hop is read from the middle of
the icon: how far up it has to be looked for to match the resting dock.
--arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

from PIL import Image

import perf_smoke as P
from motion_smoke import state

DIFF = 24                 # a channel this far from the resting dock has changed


def aim(d, x, y):
    """The pointer sent to (x, y) with no settling pause after it: a glide is
    a quarter of a second, and the move helpers wait longer than that."""
    if getattr(d.g, 'absolute', False):
        P.put(d, x, y)
        return
    dx, dy = x - P.S._pos[0], y - P.S._pos[1]
    while dx or dy:                         # paced as shell_shot.move is: PS/2 drops a burst
        sx, sy = max(-40, min(40, dx)), max(-40, min(40, dy))
        d.g.qmp.cmd('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': sx}},
                                                {'type': 'rel', 'data': {'axis': 'y', 'value': sy}}])
        dx -= sx; dy -= sy
        time.sleep(0.02)
    P.S._pos[0], P.S._pos[1] = x, y


def shot(d, out, name):
    return Image.open(d.g.capture(out / f'{name}.ppm')).convert('RGB')


def changed(a, b, x, y):
    pa, pb = a.getpixel((x, y)), b.getpixel((x, y))
    return max(abs(pa[i] - pb[i]) for i in range(3)) > DIFF


def plate_span(img, rest, row, x0, x1):
    """The columns of `row` in [x0, x1) that differ from the resting dock."""
    cols = [x for x in range(x0, x1) if changed(img, rest, x, row)]
    return (cols[0], cols[-1]) if cols else None


def raised(img, rest, tile):
    """How far the tile's icon is above where it rests, in px: the shift that
    best lines the middle of the icon up with the resting dock. (The plate
    behind it may have lit meanwhile; the icon's own middle has not changed.)"""
    x, y, w, h = tile
    cols, rows = range(x + 14, x + w - 14), range(y + 8, y + 26)
    def cost(dy):
        return sum(max(abs(p - q) for p, q in zip(img.getpixel((cx, cy - dy)), rest.getpixel((cx, cy))))
                   for cy in rows for cx in cols)
    return min(range(0, 15), key=cost)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-dock-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 300); d.g.pump(3)
        _, _, dock = state(d)
        tiles = sorted(dock.items(), key=lambda kv: kv[1][0])
        print('tiles: ' + ', '.join(f'{a} {r}' for a, r in tiles), flush=True)
        assert len(tiles) >= 3, f'the dock published too few tiles to test: {dock}'
        rest = shot(d, out, 'rest')
        x0 = tiles[0][1][0] - 12
        x1 = tiles[-1][1][0] + tiles[-1][1][2] + 12
        # just under the icons, on the plate's straight sides -- above the arcs
        # of its bottom corners, where where its edge reads depends on the glass
        row = tiles[0][1][1] + tiles[0][1][3] - 12
        (a_app, a), (c_app, c) = tiles[0], tiles[2]

        # PLATE: under the tile the pointer is over...
        P.move(d, a[0] + a[2] // 2, a[1] + a[3] // 2); d.g.pump(1.5)
        span = plate_span(shot(d, out, 'plate-a'), rest, row, x0, x1)
        assert span and abs(span[0] - a[0]) <= 4 and abs(span[1] - (a[0] + a[2] - 1)) <= 4, \
            f'over {a_app} {a}, the plate was at {span}'
        print(f'plate: under {a_app} at {span}', flush=True)
        # ...and gliding to a tile two along, captured from the moment it is sent
        aim(d, c[0] + c[2] // 2, c[1] + c[3] // 2)
        spans, end = [], time.monotonic() + 3
        while time.monotonic() < end:
            img = shot(d, out, 'sample')            # one file, written over
            s = plate_span(img, rest, row, x0, x1)
            spans.append(s)
            if s and a[0] + 4 < s[0] < c[0] - 4 and not (out / 'plate-between.png').exists():
                img.save(out / 'plate-between.png')
            if s and abs(s[0] - c[0]) <= 4 and abs(s[1] - (c[0] + c[2] - 1)) <= 4:
                break
        d.g.pump(1)
        img = shot(d, out, 'plate-c')
        span = plate_span(img, rest, row, x0, x1)
        assert span and abs(span[0] - c[0]) <= 4 and abs(span[1] - (c[0] + c[2] - 1)) <= 4, \
            f'over {c_app} {c}, the plate was at {span}'
        # one plate, not two: inside the icon button's own box (4 px in) the
        # plate's fill, not the button's hover shade (x1.12: some 5 levels)
        mid = c[1] + 20
        outer, inner = img.getpixel((c[0] + 2, mid)), img.getpixel((c[0] + 6, mid))
        assert max(abs(p - q) for p, q in zip(outer, inner)) <= 2, \
            f'a second plate inside the first: {outer} around the button, {inner} in it'
        between = [s for s in spans if s and a[0] + 4 < s[0] < c[0] - 4]
        assert between, f'the plate jumped from {a_app} to {c_app}: {spans}'
        print(f'plate: glided to {c_app}, caught on the way at {between[0][0]} '
              f'(between {a[0]} and {c[0]}; {len(spans)} captures)', flush=True)

        # HOP: a tile whose app is not running, clicked; the pointer then leaves
        P.move(d, 640, 300); d.g.pump(1.5)
        rest = shot(d, out, 'rest-2')
        app, t = next((ap_, r) for ap_, r in tiles if ap_ == 'files.elf') if 'files.elf' in dock else tiles[0]
        n0 = len(d.g.seen)
        P.click(d, t[0] + t[2] // 2, t[1] + t[3] // 2)
        P.move(d, 640, 300)
        heights, end = [], time.monotonic() + 6
        while time.monotonic() < end:
            img = shot(d, out, 'sample')
            heights.append(raised(img, rest, t))
            if heights[-1] == max(heights) and heights[-1] >= 6:
                img.save(out / 'hop-top.png')       # the highest seen, for the record
            if max(heights) >= 6 and heights[-1] == 0:
                break                               # up and back down: seen
        assert max(heights) >= 6, f'the {app} icon never left its place while the app started: {heights}'
        d.g.wait(b'first frame presented', 60, start=n0); d.g.pump(5)
        settled = raised(shot(d, out, 'hop-settled'), rest, t)
        assert settled == 0, f'with its window up, the {app} icon is still {settled} px up'
        print(f'hop: {app} up in {sum(h > 0 for h in heights)} of {len(heights)} captures, '
              f'{max(heights)} px at most; at rest once its window was up', flush=True)
    finally:
        d.g.close()
    print('PASS: the dock\'s plate glides from tile to tile, and an icon hops while its app starts', flush=True)


if __name__ == '__main__':
    main()
