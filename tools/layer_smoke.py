#!/usr/bin/env python3
"""Layers: the compositor moves a piece of a window while its app sleeps.

Boots a disposable desktop, opens the Terminal and runs layerdemo
(user/tests/layerdemo): a window painted once, and a card painted once into a
layer of it, hanging below the window's frame. Every 1.5 s the app sends the
card in or out with one call and goes back to sleep -- it never draws again.
Asserts, from the compositor's own account (`perf windows` layer lines, `perf`):

  shown     the layer is there, presented, with the window
  moves     sampled mid-transition, the card is part-way -- between its
            opacity and scale at rest -- while the app sleeps: the compositor
            moved it
  frames    while it moves, frames come at the display's rate (interval p95
            within a few ms of the period), with the app presenting nothing
  press     a click on the card BELOW the window -- where only the layer is --
            reaches the app, which sends the card away
  gone      when the app exits, its layer goes with it

and photographs the card in and mid-motion. --arch aarch64 runs it on arm64
under HVF; --graphics virgl composes it on the host's GPU (Linux).
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect

DEMO = '/data/apps/layerdemo/layerdemo.elf'


def layers(d):
    text = d.g.command('perf windows', b'perf: end', 60)
    out = []
    for line in text.splitlines():
        if line.startswith('perf: layer '):
            pairs = dict(kv.split('=', 1) for kv in line[12:].split() if '=' in kv)
            l = {k: int(pairs[k]) for k in ('id', 'x', 'y', 'w', 'h', 'alpha', 'scale', 'moving', 'shown')}
            l['to'] = [int(v) for v in pairs['to'].split(',')]
            l['app'] = pairs['app']
            out.append(l)
    return out


def frost(d):
    """(made, kept): the compositor's layer frosts blurred, and laid back unblurred"""
    text = d.g.command('perf windows', b'perf: end', 60)
    for line in text.splitlines():
        if line.startswith('perf: frost '):
            pairs = dict(kv.split('=', 1) for kv in line[12:].split() if '=' in kv)
            return int(pairs['made']), int(pairs['kept'])
    return None


def until(d, what, cond, seconds=20.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        d.g.pump(0.05)
    raise AssertionError(f'{what}: not within {seconds:.0f} s')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio',
                    help='x86_64 only: virgl composes on the host GPU (Linux)')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-layer-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_TERMINAL_X, P.DOCK_Y)
        d.g.wait(b'Terminal: first frame presented'); d.g.pump(3)
        # a first shell greets with the prompt wizard (shell/prompt/wizard.c): q leaves it
        d.g.pump(2); d.key('q'); d.g.pump(2)
        d.type(DEMO + ' 1500 6\n')
        try:
            W = until(d, 'the demo window', lambda: state(d)[0].get('Layer demo'))
        except AssertionError:
            print(f"(the Terminal then: {d.g.capture(out / 'term.ppm')})", flush=True)
            raise
        L = until(d, 'its layer, presented', lambda: [l for l in layers(d) if l['shown']])
        print(f"shown: a layer {L[0]['w']}x{L[0]['h']} with the window at {W['x']},{W['y']}", flush=True)

        # MOVES: catch it between rest states while the app sleeps
        d.g.command('perf reset', b'perf: reset', 60)
        between = None
        end = time.monotonic() + 8
        while time.monotonic() < end and not between:
            for l in layers(d):
                if l['moving'] and 0 < l['alpha'] < 255 and 940 < l['scale'] < 1000:
                    between = l
            if not between:
                d.g.pump(0.02)
        assert between, 'never caught the card part-way through a transition'
        mid = d.g.capture(out / 'mid.ppm')
        print(f"moves: part-way at alpha {between['alpha']} scale {between['scale']} ({mid})", flush=True)

        # FRAMES: over a whole transition or two, at the display's rate
        d.g.pump(3.2)
        s = P.perf(d)
        period = s['period_us'] / 1000
        p95 = s['interval_p95_us'] / 1000
        win = s.get('windows', {})
        demo_presents = sum(v.get('presents', 0) for k, v in win.items() if k.startswith('Layer'))
        print(f"frames: {s['frames']} frames, interval p50 {s['interval_p50_us'] / 1000:.2f} p95 {p95:.2f} ms "
              f"at a {period:.2f} ms beat; the app presented {demo_presents} times", flush=True)
        assert s['frames'] >= 10, f'too few frames for two transitions: {s["frames"]}'
        assert demo_presents == 0, 'the app presented while its layer moved'
        if args.arch == 'aarch64':
            assert p95 <= period + 4.0, f'frames came late while a layer moved: p95 {p95:.2f} at {period:.2f}'

        # PRESS: once it is in and still, click the part below the window
        def resting_in():
            ls = [l for l in layers(d) if l['shown'] and not l['moving'] and l['alpha'] == 255]
            return ls[0] if ls else None
        l = until(d, 'the card in and still', resting_in, 10)
        shot = d.g.capture(out / 'in.ppm')
        frame = frame_rect(state(d)[0]['Layer demo'])
        bx = l['x'] + l['w'] // 2
        by = max(frame[1] + frame[3] + 20, l['y'] + l['h'] - 30)
        assert by < l['y'] + l['h'], f'the card does not reach below the window: {l} {frame}'
        P.click(d, bx, by)
        gone = until(d, 'the press to send the card away', lambda: [x for x in layers(d) if x['to'][2] == 0], 4)
        print(f"press: a click at {bx},{by}, below the window, reached the app ({shot})", flush=True)

        # PHOTO: slowed down, the card half way in -- what a layer's motion looks like
        d.g.command('perf slowmo 6', b'perf: slowmo', 60)
        caught = None
        end = time.monotonic() + 12
        while time.monotonic() < end and not caught:
            for x in layers(d):
                if x['moving'] and 90 < x['alpha'] < 170 and x['to'][2] == 255:
                    caught = x
            if not caught:
                d.g.pump(0.02)
        if caught:
            slow = d.g.capture(out / 'slow.ppm')
            print(f"photo: half way in at alpha {caught['alpha']} scale {caught['scale']} ({slow})", flush=True)
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)

        # GONE: the app ends after its cycles; its layer goes with it
        until(d, 'the demo to end', lambda: not state(d)[0].get('Layer demo'), 30)
        d.g.pump(1)
        left = layers(d)
        assert not left, f'layers outlived their window: {left}'
        print('gone: the app ended and its layer with it', flush=True)
        print('PASS: a layer the app painted once is moved by the compositor at the display rate, takes the '
              'pointer outside its window, and goes with it')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
