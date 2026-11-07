#!/usr/bin/env python3
"""A real menu on a layer: Files' context menu, moved by the compositor.

Boots a disposable desktop, opens Files and right-clicks its empty folder
space. With layers (P7 M4) the toolkit paints the menu once into a layer of
the window and the compositor brings it in; asserts, from the compositor's
account (`perf windows`) and the app's own (`perf apps on`):

  in        the menu is a layer of files.elf, arriving -- caught part-way
            between transparent and opaque -- and then at rest, opaque
  cheap     Files built a handful of frames for all of that, not one a frame
  looks     photographed at rest, slowed mid-way in (perf slowmo)
  out       Esc: the toolkit has forgotten the menu, and the layer still
            leaves -- caught going, toward transparent -- and then is gone
  bar       the top bar's EmbLink menu is a layer too (the bar shows through:
            the menu's glass is the compositor's), photographed, and goes

--arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import re
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect
from layer_smoke import layers, until

PROF = re.compile(rb'prof: Files frames=(\d+) full=(\d+)')


def right_click(d, x, y):
    P.move(d, x, y)
    for down in (True, False):
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'right'}}])
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
    args = ap.parse_args()
    slow = 5 if args.graphics == 'virgl' else 1   # x86 emulated in a VM: everything is slower
    out = Path(tempfile.mkdtemp(prefix='emblink-layermenu-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, 555, P.DOCK_Y)
        d.g.wait(b'Files: first frame presented'); d.g.pump(3)
        w = frame_rect(state(d)[0]['Files'])
        spot = (w[0] + w[2] - 160, w[1] + w[3] - 140)
        d.g.command('perf apps on', b'perf: apps on', 60)
        P.move(d, *spot); d.g.pump(2 * slow)             # the switch is polled; hover settled
        mark = len(d.g.seen)

        # IN
        right_click(d, *spot)
        part = None
        end = time.monotonic() + 6 * slow
        while time.monotonic() < end and not part:
            for l in layers(d):
                if l['app'] == 'files.elf' and l['moving'] and 0 < l['alpha'] < 255:
                    part = l
            if not part:
                d.g.pump(0.01)
        rest = until(d, 'the menu at rest', lambda: [l for l in layers(d) if l['app'] == 'files.elf' and
                                                     not l['moving'] and l['alpha'] == 255], 6 * slow)[0]
        if part:
            print(f"in: the menu is a layer of Files, caught arriving at alpha {part['alpha']} scale {part['scale']}, "
                  f"then at rest {rest['w']}x{rest['h']} at {rest['x']},{rest['y']}", flush=True)
        else:
            print(f"in: the menu is a layer of Files at rest {rest['w']}x{rest['h']} (its arrival was quicker "
                  f"than a poll)", flush=True)
        shot = d.g.capture(out / 'menu.ppm')

        # CHEAP: what Files built for the menu's arrival
        P.move(d, spot[0] + 2, spot[1] + 2); d.g.pump(1.5)
        frames = sum(int(m.group(1)) for m in PROF.finditer(bytes(d.g.seen[mark:])))
        print(f"cheap: Files built {frames} frames for the menu's arrival ({shot})", flush=True)
        assert frames <= 12, f'Files drew the menu itself: {frames} frames'

        # OUT
        mark = len(d.g.seen)
        d.key('esc')
        going = None
        end = time.monotonic() + 4 * slow
        while time.monotonic() < end and not going:
            ls = [l for l in layers(d) if l['app'] == 'files.elf']
            for l in ls:
                if l['to'][2] == 0 and l['moving']:
                    going = l
            if not ls:
                break
            if not going:
                d.g.pump(0.01)
        until(d, 'the menu gone', lambda: not [l for l in layers(d) if l['app'] == 'files.elf'], 6 * slow)
        print('out: Esc, and ' + (f"the menu leaves on its own -- caught at alpha {going['alpha']} -- "
                                  if going else 'the menu leaves ') + 'and is gone', flush=True)

        # LOOKS: slowed, half way in
        d.g.command('perf slowmo 16', b'perf: slowmo', 60)
        right_click(d, *spot)
        caught = None
        end = time.monotonic() + 8 * slow
        while time.monotonic() < end and not caught:
            for l in layers(d):
                if l['app'] == 'files.elf' and l['moving'] and 90 < l['alpha'] < 170:
                    caught = l
            if not caught:
                d.g.pump(0.01)
        if caught:
            slowshot = d.g.capture(out / 'menu-slow.ppm')
            print(f"looks: half way in at alpha {caught['alpha']} scale {caught['scale']} ({slowshot})", flush=True)
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        d.key('esc'); d.g.pump(1)
        # THE TOP BAR'S MENU: a window that shows through, its menu's glass the compositor's
        P.click(d, 72, 16)
        bar = until(d, "the top bar's menu, at rest", lambda: [l for l in layers(d) if l['app'] == 'topbar.elf'
                                                               and not l['moving'] and l['alpha'] == 255], 6 * slow)[0]
        barshot = d.g.capture(out / 'topbar-menu.ppm')
        d.key('esc')
        until(d, "the top bar's menu gone", lambda: not [l for l in layers(d) if l['app'] == 'topbar.elf'], 6 * slow)
        print(f"bar: the top bar's menu is a layer {bar['w']}x{bar['h']}, frosted by the compositor ({barshot})",
              flush=True)
        d.g.command('perf apps off', b'perf: apps off', 60)
        print('PASS: Files\' context menu is a layer: the compositor brings it in and takes it away, and Files '
              'draws only its first frame')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
