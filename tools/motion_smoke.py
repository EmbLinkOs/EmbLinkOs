#!/usr/bin/env python3
"""Window motion on springs, watched in flight.

Boots a disposable desktop, opens Settings, and drives the motions a hand
starts -- the window's minimise light, its dock tile -- while sampling the
compositor's ghosts over the serial console (`perf windows` lists every ghost
in the air: its kind, rect, opacity and target). It asserts:

  park       a minimised window flies into its own app's dock tile (the rect
             the desktop published), shrinking on the way and fading only as it
             reaches the tile, and is gone from the screen when it lands
  restore    clicking the tile flies it back out to where it was
  interrupt  minimised and clicked back before it lands, it turns around from
             where it had got to -- never jumping to the tile first -- and
             lands where it started
  together   two windows minimised at once (GUI+M twice) are two ghosts in
             the air at once
  scroll     one notch of the wheel over Settings glides: several frames of
             motion, not one jump

and captures frames mid-flight. --arch aarch64 runs it on arm64 under HVF.
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P

MIN_LIGHT = (52, 16)        # the minimise light, from the window's top-left (ui/dsl/em.c)


def state(d):
    """Windows by title, ghosts in the air, and the dock's tiles by app."""
    text = d.g.command('perf windows', b'perf: end', 60)
    wins, ghosts, dock = {}, [], {}
    for line in text.splitlines():
        if not line.startswith('perf: '):
            continue
        kind, _, rest = line[6:].partition(' ')
        pairs = dict(kv.split('=', 1) for kv in rest.split() if '=' in kv)
        if kind == 'window':
            wins[pairs['title'].replace('_', ' ')] = {k: int(pairs[k]) for k in ('x', 'y', 'w', 'h')}
        elif kind == 'ghost':
            g = {k: int(pairs[k]) for k in ('x', 'y', 'w', 'h', 'alpha')}
            g['kind'], g['app'] = pairs['kind'], pairs['app']
            g['to'] = [int(v) for v in pairs['to'].split(',')]
            g['t'] = time.monotonic()
            ghosts.append(g)
        elif kind == 'dock':
            dock[pairs['app']] = [int(pairs[k]) for k in ('x', 'y', 'w', 'h')]
    return wins, ghosts, dock


def watch(d, app, seconds=4.0):
    """Sample the ghosts of `app` until none is left; returns the samples."""
    seen, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        _, ghosts, _ = state(d)
        mine = [g for g in ghosts if g['app'] == app]
        if not mine:
            break
        seen.extend(mine)
    return seen


def until_back(d, title, app, seconds=10.0):
    """Wait for `title` to be on screen with no ghost of `app` left -- an
    emulator may take a while to act on the click at all. Returns the ghost
    samples seen on the way and the window."""
    seen, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        wins, ghosts, _ = state(d)
        mine = [g for g in ghosts if g['app'] == app]
        seen.extend(mine)
        if title in wins and not mine:
            return seen, wins[title]
    return seen, None


def frame_rect(win):
    """A window's frame as the ghosts report theirs: x, y, w, h (the content
    sits under no kernel bar here -- EmUI windows are chromeless)."""
    return [win['x'], win['y'], win['w'], win['h']]


def near(a, b, tol=3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def tap(d, x, y=None):
    """A click with no settling pause: what an impatient hand does. With no
    position, where the pointer already is."""
    if x is not None:
        if getattr(d.g, 'absolute', False):
            P.put(d, x, y)
        else:                       # a relative mouse: let the guest catch up
            P.S.move(d.g.qmp, x, y); d.g.pump(0.4)
    P.press(d, True); time.sleep(0.03); P.press(d, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='emblink-motion-'))
    print(f'Logs and captures: {out}', flush=True)
    d = P.boot(args, out)
    try:
        d.login(setup=True)
        P.S._pos[0] = None
        P.move(d, 640, 400); d.g.pump(3)
        P.click(d, P.DOCK_SETTINGS_X, P.DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(3)

        wins, _, dock = state(d)
        start = frame_rect(wins['Settings'])
        tile = dock.get('settings.elf')
        assert tile, f'the desktop published no tile for settings.elf: {dock}'
        print(f'Settings at {start}; its dock tile at {tile}', flush=True)
        light = (start[0] + MIN_LIGHT[0], start[1] + MIN_LIGHT[1])

        # SCROLL: a notch glides -- the app presents a run of frames, not one
        P.move(d, start[0] + start[2] - 45, start[1] + start[3] // 2); d.g.pump(1)
        d.g.command('perf reset', b'perf: reset', 60)
        d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': 'wheel-down'}},
                                                {'type': 'btn', 'data': {'down': False, 'button': 'wheel-down'}}])
        d.g.pump(1.5)
        presents = P.perf(d).get('windows', {}).get('Settings', {}).get('presents', 0)
        assert presents >= 3, f'one notch was {presents} frame(s) of Settings: it jumped, it did not glide'
        print(f'scroll: one notch glided over {presents} frames', flush=True)

        # PARK: into its own tile, smaller and fainter on the way
        P.click(d, *light)
        flight = watch(d, 'settings.elf')
        assert flight, 'minimising Settings put no ghost in the air'
        assert all(g['kind'] == 'park' for g in flight), f'expected a park: {flight}'
        assert all(g['to'] == tile for g in flight), f'the ghost was not aimed at the tile {tile}: {flight[0]["to"]}'
        widths = [g['w'] for g in flight]
        assert widths == sorted(widths, reverse=True), f'the ghost did not shrink steadily: {widths}'
        alphas = [g['alpha'] for g in flight]
        assert alphas == sorted(alphas, reverse=True), f'the ghost did not fade steadily: {alphas}'
        # a window all the way down: opaque until the last tenth of the trip
        big = [g for g in flight if g['w'] > tile[2] + (start[2] - tile[2]) // 8]
        assert all(g['alpha'] == 255 for g in big), f'it faded before it reached the dock: {big}'
        wins, ghosts, _ = state(d)
        assert 'Settings' not in wins and not ghosts, 'Settings is still on the screen after parking'
        print(f'park: {len(flight)} samples in flight, width {widths[0]} -> {widths[-1]}, '
              f'alpha {alphas[0]} -> {alphas[-1]}; landed in the tile', flush=True)

        # RESTORE: out of the tile, back where it was
        tap(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        flight, win = until_back(d, 'Settings', 'settings.elf')
        assert all(g['kind'] == 'unpark' for g in flight), f'expected an unpark: {flight}'
        assert all(near(g['to'], start) for g in flight), f'the ghost was not aimed home {start}: {flight[0]["to"]}'
        assert win and near(frame_rect(win), start, 0), f'Settings did not come back where it was: {win} vs {start}'
        print(f'restore: {len(flight)} samples, back at {frame_rect(win)}', flush=True)

        # INTERRUPT: clicked back mid-flight, it turns around where it is --
        # the compositor says where (it is the one case a curve cannot do).
        # The pointer waits on the tile and GUI+M sends the window: no pointer
        # travel between the two, which a relative mouse would make too slow.
        P.move(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        assert 'Settings' in state(d)[0], 'Settings is not back on screen to be minimised again'
        n0 = len(d.g.seen)
        d.key('meta_l-m')                   # (sends, then waits 0.2 s)
        tap(d, None)
        d.g.wait(b'settings.elf turned around in flight', 20, start=n0)
        line = d.g.seen[n0:].decode(errors='replace').split('settings.elf turned around in flight at ')[1].split('\n')[0]
        at = line.split(' (')[0]
        xy, wh = at.split(' ')
        turned_w = int(wh.split('x')[0])
        assert '(park -> unpark)' in line, f'expected a park turned into an unpark: {line}'
        assert tile[2] < turned_w < start[2], f'it did not turn around mid-air: at width {turned_w} (tile {tile[2]}, window {start[2]})'
        _, win = until_back(d, 'Settings', 'settings.elf')
        assert win and near(frame_rect(win), start, 0), \
            f'interrupted, Settings did not land where it started: {win} vs {start}'
        print(f'interrupt: turned around in flight at {at}, landed at {frame_rect(win)}', flush=True)

        # TOGETHER: two windows minimised at once are two ghosts
        n0 = len(d.g.seen)
        P.click(d, P.DOCK_TERMINAL_X, P.DOCK_Y)
        d.g.wait(b'Terminal: first frame presented', start=n0); d.g.pump(3)
        # GUI+M twice: the Terminal in front goes, then Settings, which is in
        # front once the Terminal is in the air -- no pointer travel between
        d.key('meta_l-m')
        d.key('meta_l-m')
        both = set()
        for _ in range(6):
            _, ghosts, _ = state(d)
            both |= {g['app'] for g in ghosts if g['kind'] == 'park'}
            if len(both) == 2 or not ghosts:
                break
        assert both == {'term.elf', 'settings.elf'}, f'two windows minimised at once were not two ghosts: {both}'
        watch(d, 'settings.elf'); watch(d, 'term.elf')
        print('together: two ghosts in the air at once', flush=True)

        # FRAMES MID-FLIGHT, for the record: Settings restored, then parked
        # again ten times slower (`perf slowmo`, the same motion) and
        # photographed on the way into its tile
        tap(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        until_back(d, 'Settings', 'settings.elf'); d.g.pump(1)
        d.g.command('perf slowmo 10', b'perf: slowmo', 60)
        d.key('meta_l-m')
        shots, end = [], time.monotonic() + 30
        while len(shots) < 8 and time.monotonic() < end:
            _, ghosts, _ = state(d)
            mine = [g for g in ghosts if g['app'] == 'settings.elf']
            if not mine:
                break
            path = d.g.capture(out / f'flight-{len(shots)}.ppm')
            shots.append(f"{path} (ghost {mine[-1]['w']}x{mine[-1]['h']} alpha {mine[-1]['alpha']})")
            time.sleep(0.4)
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        print('captures:\n  ' + '\n  '.join(shots), flush=True)
    finally:
        d.g.close()
    print('PASS: a window parks into its own dock tile and comes back out of it, turns around '
          'mid-flight when interrupted, and two windows move at once', flush=True)


if __name__ == '__main__':
    main()
