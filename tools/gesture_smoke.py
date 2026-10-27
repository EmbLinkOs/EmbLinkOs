#!/usr/bin/env python3
"""Three fingers: the spaces under the hand, and the overview.

Boots a disposable desktop with a touchscreen beside its tablet
(virtio-multitouch-pci), puts Settings on the first space and the Terminal on
the second, and asserts:

  clicks   the pointer (a tablet on arm64, a mouse on x86) still clicks with
           a touchscreen on the machine, which QEMU hands the clicks to
  hand     three fingers moving left hold both spaces under the hand: Settings
           (going) and the Terminal (coming) follow the fingers one to one,
           side by side, the space shown unchanged; the fingers, on Settings'
           bar, neither press, drag nor move the pointer; at rest they compose
           (nearly) nothing
  back     lifted at rest short of a third of the screen, they spring back
  take     lifted past it, the next space is taken
  flick    a short fast swipe right, over before it is far, takes it back
  edge     toward a space that is not there the hand meets a rubber band
           (never past 120 px), and lets go
  up/down  three fingers up open the overview; down close it

and photographs the two spaces under the hand. --arch aarch64 runs it on arm64
under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P
from motion_smoke import state, frame_rect, settle
from overview_smoke import settled

W, H = 1280, 800
MAX = 0x7fff                                   # QEMU's touch axes run 0..0x7fff


def unit(px, span):
    """Screen px -> the touchscreen's units, so the guest's u * span / (MAX + 1)
    comes back to px exactly."""
    return min(MAX, -(-px * (MAX + 1) // span))


def mtt(kind, slot, tid, axis, value):
    return {'type': 'mtt', 'data': {'type': kind, 'slot': slot, 'tracking-id': tid, 'axis': axis, 'value': value}}


class Hand:
    """Fingers on the glass, in screen px, a slot each. One QMP batch is one
    frame of contacts (QEMU syncs after each)."""

    def __init__(self, d):
        self.d, self.at = d, []

    def _frame(self, kind):
        ev = []
        for i, (x, y) in enumerate(self.at):
            ev += [mtt(kind, i, 10 + i, 'x', 0),
                   mtt('data', i, 10 + i, 'x', unit(x, W)), mtt('data', i, 10 + i, 'y', unit(y, H))]
        self.d.g.qmp.cmd('input-send-event', events=ev)

    def down(self, points):
        self.at = list(points); self._frame('begin')

    def move(self, dx, dy=0, steps=None, pause=0.03):
        """The hand dx, dy further, in even steps, as a hand moves. Paced by
        the clock alone: reading the serial line takes 100 ms a time, which
        no flick survives."""
        steps = steps or max(1, max(abs(dx), abs(dy)) // 16)
        done = [0, 0]
        for k in range(1, steps + 1):
            sx, sy = dx * k // steps - done[0], dy * k // steps - done[1]
            done = [done[0] + sx, done[1] + sy]
            self.at = [(x + sx, y + sy) for x, y in self.at]
            self._frame('update')
            time.sleep(pause)

    def up(self):
        self.d.g.qmp.cmd('input-send-event', events=[mtt('end', i, -1, 'x', 0) for i in range(len(self.at))])
        self.at = []


def held(d, seconds=10.0):
    """The slide ghosts, held where the hand put them, by app: two equal samples."""
    last, end = None, time.monotonic() + seconds
    while time.monotonic() < end:
        _, ghosts, _ = state(d)
        mine = {g['app']: (g['kind'], g['x'], g['y']) for g in ghosts if g['kind'].startswith('slide')}
        if mine and mine == last:
            return mine
        last = mine
    raise AssertionError(f'no held slide ghosts: {last}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-gesture-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out, extra=('-device', 'virtio-multitouch-pci'))
    try:
        d.login(setup=True)
        assert b'touchscreen' in d.g.seen, 'the guest found no touchscreen'
        d.g.command('perf effects full', b'perf: effects', 60)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)

        # CLICKS: a click on the dock still opens an app with a touchscreen there
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)
        n0 = len(d.g.seen)
        P.click(d, P.DOCK_TERMINAL_X, P.DOCK_Y)
        d.g.wait(b'Terminal: first frame presented', start=n0); d.g.pump(3)
        print('clicks: two apps opened from the dock, a touchscreen beside the pointer', flush=True)
        d.key('meta_l-shift-2')                        # the Terminal to the second space
        end = time.monotonic() + 15
        while time.monotonic() < end:
            wins, ghosts, _ = state(d)
            if wins.get('Terminal', {}).get('space') == 1 and not ghosts:
                break
        assert wins['Terminal']['space'] == 1 and wins['Settings']['space'] == 0 and d.space == 0, wins
        S, T = frame_rect(wins['Settings']), frame_rect(wins['Terminal'])
        ptr = d.ptr
        hand = Hand(d)
        bar = (S[0] + S[2] // 2, S[1] + 18)            # where Settings is dragged by

        # HAND: three fingers on Settings' bar, moving left
        n0 = len(d.g.seen)
        hand.down([(bar[0] - 60, bar[1]), (bar[0], bar[1]), (bar[0] + 60, bar[1])]); d.g.pump(0.2)
        hand.move(-200)
        d.g.wait(b'compositor: swipe from space 1 toward 2', 20, start=n0)
        ghosts = held(d)
        wins, _, _ = state(d)
        assert ghosts.get('settings.elf', ('',))[0] == 'slide-in' and abs(ghosts['settings.elf'][1] - (S[0] - 200)) <= 3, \
            f'Settings did not follow the hand 200 px left: {ghosts} (home {S})'
        assert ghosts.get('term.elf', ('',))[0] == 'slide-out' and abs(ghosts['term.elf'][1] - (T[0] - 200 + W)) <= 3, \
            f'the Terminal did not come in beside it: {ghosts} (home {T})'
        assert d.space == 0, f'the space changed under the hand: {d.space}'
        assert frame_rect(wins['Settings']) == S, f'the first finger dragged Settings: {wins["Settings"]} vs {S}'
        assert d.ptr == ptr, f'the fingers moved the pointer: {d.ptr} from {ptr}'
        print(f'hand: Settings at x {ghosts["settings.elf"][1]} (home {S[0]}), the Terminal at '
              f'x {ghosts["term.elf"][1]}, space unchanged, pointer still', flush=True)
        # STILL: a hand at rest is not motion -- the spaces under it compose (nearly) nothing
        d.g.command('perf reset', b'perf: reset', 60); d.g.pump(2)
        composes = P.perf(d).get('composes', 0)
        assert composes <= 4, f'a hand at rest composed {composes} times in two seconds'
        print(f'still: {composes} composes in two seconds of a hand at rest', flush=True)
        # BACK: lifted at rest, short of a third
        d.g.pump(0.3)
        hand.up()
        d.g.wait(b'compositor: swipe let go of space 1', 20, start=n0)
        _, wins = settle(d, 0)
        assert frame_rect(wins['Settings']) == S and d.ptr == ptr, f'Settings did not spring home: {wins["Settings"]}'
        print('back: let go short of a third, both spaces sprang back', flush=True)

        # TAKE: past a third, photographed on the way
        n0 = len(d.g.seen)
        hand.down([(500, 400), (560, 400), (620, 400)]); d.g.pump(0.2)
        hand.move(-560)
        d.g.wait(b'compositor: swipe from space 1 toward 2', 20, start=n0)
        ghosts = held(d)
        assert abs(ghosts['term.elf'][1] - (T[0] - 560 + W)) <= 3, f'the Terminal is not under the hand: {ghosts}'
        shot = d.g.capture(out / 'swipe.ppm')
        d.g.pump(0.3)
        hand.up()
        d.g.wait(b'compositor: swipe took space 2', 20, start=n0)
        _, wins = settle(d, 1)
        assert frame_rect(wins['Terminal']) == T and wins['Settings']['space'] == 0, f'the spaces moved windows: {wins}'
        print(f'take: past a third, the second space taken ({shot})', flush=True)

        # FLICK: right, short and fast -- back to the first
        n0 = len(d.g.seen)
        hand.down([(500, 400), (560, 400), (620, 400)]); d.g.pump(0.2)
        hand.move(120, steps=4, pause=0.012)
        hand.up()
        d.g.wait(b'compositor: swipe took space 1', 20, start=n0)
        _, wins = settle(d, 0)
        assert frame_rect(wins['Settings']) == S, f'Settings did not come home: {wins["Settings"]}'
        print('flick: 120 px, fast, took the first space back', flush=True)

        # EDGE: right again, toward a space before the first
        n0 = len(d.g.seen)
        hand.down([(400, 400), (460, 400), (520, 400)]); d.g.pump(0.2)
        hand.move(300)
        d.g.wait(b'compositor: swipe from space 1 toward 0', 20, start=n0)
        ghosts = held(d)
        give = ghosts['settings.elf'][1] - S[0]
        assert 0 < give <= 120 and 'term.elf' not in ghosts, f'no rubber band at the first space: {ghosts}'
        d.g.pump(0.3)
        hand.up()
        d.g.wait(b'compositor: swipe let go of space 1', 20, start=n0)
        _, wins = settle(d, 0)
        assert frame_rect(wins['Settings']) == S, f'Settings did not spring home: {wins["Settings"]}'
        print(f'edge: the hand went 300 px, the space {give} px, and sprang back', flush=True)

        # UP / DOWN: the overview
        n0 = len(d.g.seen)
        hand.down([(500, 500), (560, 500), (620, 500)]); d.g.pump(0.2)
        hand.move(0, -120)
        hand.up()
        d.g.wait(b'compositor: overview of 1 windows', 20, start=n0)
        settled(d, 'overview', 1)
        hand.down([(500, 300), (560, 300), (620, 300)]); d.g.pump(0.2)
        hand.move(0, 120)
        hand.up()
        d.g.wait(b'compositor: overview closed', 20, start=n0)
        _, wins = settle(d, 0)
        assert frame_rect(wins['Settings']) == S, f'Settings did not come home: {wins["Settings"]}'
        print('up/down: three fingers up opened the overview, down closed it', flush=True)
    finally:
        d.g.close()
    print('PASS: three fingers hold the spaces under the hand, take the next past a third or on a '
          'flick, meet a rubber band at the end, and open and close the overview', flush=True)


if __name__ == '__main__':
    main()
