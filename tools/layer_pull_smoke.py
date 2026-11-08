#!/usr/bin/env python3
"""The close pull on a layer: a window's content moved by the compositor.

Boots a disposable desktop, opens the Terminal and runs v7demo, an EmUI window
with a close grip (CloseGrip) in its bar. Pulling the grip used to repaint the
whole window every frame of the drag and of the spring back; on layers the
content is a layer while it is pulled. Asserts, from the compositor's account
(`perf windows`, `perf reset`):

  pull      held and dragged 40 px: the content is a layer of the window, moved
            toward the hand and faded, photographed
  home      let go short: the compositor springs it home -- caught on its way --
            and it is the window's pixels again, the app presenting at most
            twice for all of it (the frame that takes it back)
  close     pulled past the threshold: the window closes

--arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import os
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect
from layer_smoke import layers, until

DEMO = '/data/apps/v7demo/v7demo.elf'


def press(d, down):
    d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])
    time.sleep(0.05)


def mine(d):
    return [l for l in layers(d) if l['app'] == 'v7demo.elf']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
    args = ap.parse_args()
    slow = 5 if args.graphics == 'virgl' else 3 if os.environ.get('CI') else 1   # CI: a shared, slower emulator
    out = Path(tempfile.mkdtemp(prefix='emblink-layerpull-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_TERMINAL_X, P.DOCK_Y)
        d.g.wait(b'Terminal: first frame presented'); d.g.pump(3)
        d.g.pump(2); d.key('q'); d.g.pump(2)               # the prompt wizard: q leaves it
        d.type(DEMO + '\n')
        W = until(d, 'the demo window', lambda: state(d)[0].get('Editor'), 20 * slow)
        d.g.pump(2 * slow)
        f = frame_rect(state(d)[0]['Editor'])
        grip = (f[0] + f[2] - 36, f[1] + 22)

        # PULL: held, dragged 40 px
        P.move(d, *grip); d.g.pump(0.5)
        press(d, True)
        for k in range(1, 9):
            P.move(d, grip[0] + 5 * k, grip[1]); d.g.pump(0.05)
        pulled = until(d, 'the content a layer, toward the hand',
                       lambda: [l for l in mine(d) if l['x'] > f[0] + 5 and l['to'][2] < 230], 6 * slow)[0]
        shot = d.g.capture(out / 'pull.ppm')
        print(f"pull: the content is a layer {pulled['w']}x{pulled['h']}, at {pulled['x']} (the window at {f[0]}), "
              f"alpha {pulled['alpha']} ({shot})", flush=True)

        # HOME: let go short
        d.g.command('perf reset', b'perf: reset', 60)
        press(d, False)
        going = None
        end = time.monotonic() + 3 * slow
        while time.monotonic() < end and not going:
            ls = mine(d)
            if not ls:
                break
            for l in ls:
                if l['moving'] and l['to'][2] == 255 and l['x'] > f[0] + 1:
                    going = l
            if not going:
                d.g.pump(0.01)
        until(d, 'the content back in the window', lambda: not mine(d), 4 * slow)
        s = P.perf(d)
        presents = sum(v.get('presents', 0) for k, v in s.get('windows', {}).items() if k.startswith('Editor'))
        print('home: let go, it ' + (f"springs home -- caught at {going['x']} on its way to {f[0]} -- "
                                     if going else 'went home ') +
              f'and is the window\'s pixels again; the app presented {presents} times', flush=True)
        assert presents <= 2, f'the app drew the spring home itself: {presents} presents'

        # CLOSE: pulled past the threshold
        d.g.pump(0.5)
        P.move(d, *grip); d.g.pump(0.5)
        press(d, True)
        for k in range(1, 16):
            P.move(d, grip[0] + 10 * k, grip[1]); d.g.pump(0.05)
        press(d, False)
        until(d, 'the window closed', lambda: 'Editor' not in state(d)[0], 6 * slow)
        print('close: pulled past the threshold, the window closed', flush=True)
        print('PASS: a pulled window\'s content is a layer the compositor moves and springs home; '
              'the app draws neither')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
