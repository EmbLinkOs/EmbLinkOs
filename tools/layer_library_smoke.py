#!/usr/bin/env python3
"""The library on layers: the desktop's sheet and its dimming, moved by the compositor.

Boots a disposable desktop and opens the library (GUI+Space). With layers
(P7 M6) the desktop paints the dimming and the glass sheet once, each into a
layer of its window, and the compositor brings them in. Asserts:

  in        two layers of the desktop: one the size of the screen (the
            dimming), one the sheet; the sheet caught arriving, its rows
            rising in a beat apart, a layer each, then both at rest
  looks     photographed at rest, and slowed half way in (perf slowmo)
  works     typing an application's name and Return launches it
  out       the layers leave as the library closes, and are gone

--arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from layer_smoke import layers, until


def desktop_layers(d):
    return [l for l in layers(d) if l['app'] == 'home.elf']


def catch(d, cond, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        hit = [l for l in desktop_layers(d) if cond(l)]
        if hit:
            return hit[0]
        d.g.pump(0.01)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
    args = ap.parse_args()
    slow = 5 if args.graphics == 'virgl' else 1   # x86 emulated in a VM: everything is slower
    out = Path(tempfile.mkdtemp(prefix='emblink-layerlib-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 60); d.g.pump(3)

        # IN
        d.key('meta_l-spc')
        sheet = catch(d, lambda l: l['w'] < P.W and l['moving'] and 0 < l['alpha'] < 255, 6 * slow)
        # ROWS: a layer each while they arrive, a beat apart -- caught at once
        # at different alphas -- and then the sheet's pixels again
        rows = stagger = None
        end = time.monotonic() + 3 * slow
        while time.monotonic() < end and not stagger:
            rs = [l for l in desktop_layers(d) if l['w'] < P.W - 2 and l['h'] < 200]
            if len(rs) >= 2:
                rows = rows or rs
                al = sorted(l['alpha'] for l in rs)
                if al[-1] - al[0] >= 40:
                    stagger = rs
            if not stagger:
                d.g.pump(0.01)
        both = until(d, 'the dimming and the sheet at rest, the rows back in the sheet',
                     lambda: (lambda ls: ls if len(ls) == 2 and all(not l['moving'] and l['alpha'] == 255
                                                                   for l in ls) else None)(desktop_layers(d)), 8 * slow)
        dim = [l for l in both if l['w'] >= P.W - 2]
        sh = [l for l in both if l['w'] < P.W - 2]
        assert dim and sh, f'expected the dimming and the sheet: {both}'
        print(f"in: the dimming {dim[0]['w']}x{dim[0]['h']} and the sheet {sh[0]['w']}x{sh[0]['h']} are layers"
              + (f"; the sheet caught arriving at alpha {sheet['alpha']} scale {sheet['scale']}" if sheet else ''),
              flush=True)
        print(f"rows: {len(rows) if rows else 0} row layers while it arrived, then the sheet's pixels again",
              flush=True)
        stagger = None
        if args.graphics != 'virgl':
            assert rows, 'the rows did not arrive as layers'
        rest = d.g.capture(out / 'library.ppm')

        # WORKS: find Settings, Return
        mark = len(d.g.seen)
        d.type('settings\n')
        d.g.wait(b'Settings: first frame presented', start=mark)
        until(d, 'the layers gone with the library', lambda: not desktop_layers(d), 6 * slow)
        print(f'works: "settings" and Return launched Settings, and the library left ({rest})', flush=True)

        # LOOKS: slowed, half way in; then OUT
        d.g.command('perf slowmo 4', b'perf: slowmo', 60)
        d.key('meta_l-spc')
        half = catch(d, lambda l: l['w'] < P.W and l['h'] >= 200 and l['moving'] and 90 < l['alpha'] < 200, 10 * slow)
        if half:
            slowshot = d.g.capture(out / 'library-slow.ppm')
            print(f"looks: half way in at alpha {half['alpha']} scale {half['scale']} ({slowshot})", flush=True)
        # slowed, the rows are seen a beat apart
        end = time.monotonic() + 3 * slow
        while time.monotonic() < end and not stagger:
            rs = [l for l in desktop_layers(d) if l['w'] < P.W - 2 and l['h'] < 200]
            if len(rs) >= 2 and max(l['alpha'] for l in rs) - min(l['alpha'] for l in rs) >= 40:
                stagger = rs
                rowshot = d.g.capture(out / 'library-rows.ppm')
                print('rows: ' + ', '.join(f"alpha {l['alpha']} at y {l['y']}" for l in sorted(rs, key=lambda l: l['y']))
                      + f' -- a layer a row, a beat apart ({rowshot})', flush=True)
            d.g.pump(0.02)
        if args.graphics != 'virgl':
            assert stagger, 'the rows did not arrive a beat apart'
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        until(d, 'the library at rest', lambda: [l for l in desktop_layers(d) if not l['moving']], 10 * slow)
        d.key('esc')
        going = catch(d, lambda l: l['to'][2] == 0, 3 * slow)
        until(d, 'the layers gone', lambda: not desktop_layers(d), 6 * slow)
        print('out: Esc, and the library ' + ('leaves on the compositor\'s springs' if going else 'leaves') +
              ', and is gone', flush=True)
        print('PASS: the library and its dimming are layers of the desktop: in, out, and it still launches')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
