#!/usr/bin/env python3
"""What the desktop's animations cost the APPLICATIONS drawing them.

The compositor's own costs are perf_smoke.py's. This is the other half: with
`perf apps on`, every toolkit runtime says what each run of frames cost it --
building, laying out, painting, presenting (ui/dsl/em_prof.h). Boots a
disposable desktop and plays the motions the layers work is for, each between
markers, then sums each scene's `prof:` lines per application:

  menu      the top bar's EmbLink menu dropped in, and closed with Esc
  tooltip   Settings' close button hovered until its tooltip shows, then left
  context   a right-click in Files' empty folder space, and Esc
  launcher  the library (GUI+Space) zoomed in, and out with Esc
  dock      the pointer gliding along the dock and away
  scroll    eight wheel notches down Settings' pane and back

and prints, per scene and application: frames, how many repainted everything,
the average and dearest paint, the average build+layout, and the pixels
presented. `--arch aarch64` runs it on arm64 under HVF, where the numbers are
a real core's. `--json` also writes them to a file.
"""
import argparse
import json
import re
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect

LINE = re.compile(rb'prof: (\S+) frames=(\d+) full=(\d+) build=(\d+) layout=(\d+) paint=(\d+) '
                  rb'paint_max=(\d+) present=(\d+) kpx=(\d+) wall=(\d+)')


def scene_lines(seen, start, end):
    out = {}
    for m in LINE.finditer(bytes(seen[start:end])):
        app = m.group(1).decode()
        n, full, b, l, p, pmax, pr, kpx = (int(m.group(i)) for i in range(2, 10))
        a = out.setdefault(app, {'frames': 0, 'full': 0, 'build_layout_us': 0, 'paint_us': 0,
                                 'paint_max_us': 0, 'present_us': 0, 'kpx': 0})
        a['frames'] += n; a['full'] += full; a['kpx'] += kpx
        a['build_layout_us'] += (b + l) * n; a['paint_us'] += p * n; a['present_us'] += pr * n
        a['paint_max_us'] = max(a['paint_max_us'], pmax)
    for a in out.values():                      # totals -> averages a frame
        for k in ('build_layout_us', 'paint_us', 'present_us'):
            a[k] = round(a[k] / a['frames']) if a['frames'] else 0
    return out


def wheel(d, down, n):
    b = 'wheel-down' if down else 'wheel-up'
    for _ in range(n):
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': b}}])
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': b}}])
        time.sleep(0.12)


def right_click(d, x, y):
    P.move(d, x, y)
    for down in (True, False):
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'right'}}])
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--json', help='also write the results here')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-appprof-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    results = {}
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        wins, _, _ = state(d)
        S = frame_rect(wins['Settings'])
        d.g.command('perf apps on', b'perf: apps on')
        d.g.pump(2)                                 # the runtimes poll the switch (600 ms)

        def scene(name, body, settle=2.0):
            P.move(d, 640, 420); d.g.pump(1.5)      # off everything; a run ends at the wait
            start = len(d.g.seen)
            body()
            d.g.pump(settle)
            P.move(d, 640, 420); d.g.pump(1.5)
            results[name] = scene_lines(d.g.seen, start, len(d.g.seen))
            print(f'{name}: ' + ', '.join(f"{a} {v['frames']} frames" for a, v in results[name].items()),
                  flush=True)

        scene('menu', lambda: (P.click(d, 72, 16), d.g.pump(1.5), d.key('esc')))
        scene('tooltip', lambda: (P.move(d, S[0] + 20, S[1] + 17), d.g.pump(2.0)))
        def context():
            P.click(d, 555, P.DOCK_Y)
            d.g.wait(b'Files: first frame presented'); d.g.pump(3)
            w = frame_rect(state(d)[0]['Files'])
            right_click(d, w[0] + w[2] - 120, w[1] + w[3] - 80); d.g.pump(1.5)
            d.key('esc')
        scene('context', context)
        scene('launcher', lambda: (d.key('meta_l-spc'), d.g.pump(2.0), d.key('esc')))
        scene('dock', lambda: (P.move(d, 500, P.DOCK_Y), P.glide(d, 780, P.DOCK_Y, step=6, pause=0.02),
                               P.move(d, 640, 500)))
        def scroll():
            P.click(d, S[0] + 600, S[1] + 15); d.g.pump(1)          # Settings in front again
            P.move(d, S[0] + 600, S[1] + 400); d.g.pump(1)
            wheel(d, True, 8); d.g.pump(1); wheel(d, False, 8)
        scene('scroll', scroll)
        d.g.command('perf apps off', b'perf: apps off')
    finally:
        d.g.close()

    print(f"\n{'scene':10} {'app':10} {'frames':>6} {'full':>5} {'paint':>7} {'max':>7} {'build+lay':>9} {'kpx':>7}")
    for name, apps in results.items():
        for app, v in sorted(apps.items(), key=lambda kv: -kv[1]['frames']):
            print(f"{name:10} {app:10} {v['frames']:6} {v['full']:5} {v['paint_us']:6}u {v['paint_max_us']:6}u "
                  f"{v['build_layout_us']:8}u {v['kpx']:7}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
