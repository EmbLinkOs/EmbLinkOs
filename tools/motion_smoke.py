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
  zoom       a double click on Settings' bar fills the work area and another
             brings it back: each time the window flies between the two frames
             (a `zoom` ghost through sizes between them), landing exactly
  slide      changing workspace (GUI+2, GUI+1) slides the windows: Settings
             goes off to the left going up the spaces and comes back in from
             the left going down, landing exactly where it was; switching
             back mid-slide turns it round in flight

and captures frames mid-flight. --arch aarch64 runs it on arm64 under HVF;
--graphics virgl composes on the host's GPU (Linux, as tools/virgl_smoke.py).
"""
import argparse
import tempfile
import time
from pathlib import Path

import perf_smoke as P

MIN_LIGHT = (52, 16)        # the minimise light, from the window's top-left (ui/dsl/em.c)


def state(d):
    """Windows by title, ghosts in the air, and the dock's tiles by app; the
    workspace shown is left in d.space."""
    text = d.g.command('perf windows', b'perf: end', 60)
    wins, ghosts, dock = {}, [], {}
    for line in text.splitlines():
        if not line.startswith('perf: '):
            continue
        kind, _, rest = line[6:].partition(' ')
        pairs = dict(kv.split('=', 1) for kv in rest.split() if '=' in kv)
        if kind == 'workspace':
            d.space = int(pairs['now'])
        elif kind == 'pointer':
            d.ptr = (int(pairs['x']), int(pairs['y']))
        elif kind == 'window':
            wins[pairs['title'].replace('_', ' ')] = {k: int(pairs[k]) for k in ('x', 'y', 'w', 'h', 'space', 'z') if k in pairs}
            wins[pairs['title'].replace('_', ' ')]['drag0'] = [int(v) for v in pairs['drag0'].split(',')]
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


def settle(d, space, seconds=10.0):
    """Wait for `space` to be the one shown with nothing in the air; returns
    the ghosts seen on the way."""
    seen, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        wins, ghosts, _ = state(d)
        seen.extend(ghosts)
        if d.space == space and not ghosts:
            return seen, wins
    raise AssertionError(f'space {space} did not settle: shown {d.space}, ghosts {ghosts}')


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
    ap.add_argument('--graphics', choices=('virtio', 'virgl'), default='virtio', help='x86_64 only')
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
        # a jump is one frame; under the Lima VM's nested emulation a 180 ms
        # glide is drawn in two or three
        assert presents >= (2 if args.graphics == 'virgl' else 3), \
            f'one notch was {presents} frame(s) of Settings: it jumped, it did not glide'
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
        # Four times slower, so an emulator's late click still finds it in the
        # air (at full speed under TCG the click met it a few pixels from the tile).
        P.move(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        assert 'Settings' in state(d)[0], 'Settings is not back on screen to be minimised again'
        d.g.command('perf slowmo 4', b'perf: slowmo', 60)
        n0 = len(d.g.seen)
        d.key('meta_l-m')                   # (sends, then waits 0.2 s)
        tap(d, None)
        d.g.wait(b'settings.elf turned around in flight', 20, start=n0)
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
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

        # ZOOM: a double click on its bar, and back
        def double_click(x, y):
            P.move(d, x, y)
            end = time.monotonic() + 10                # the compositor has the pointer there
            while time.monotonic() < end and (abs(d.ptr[0] - x) > 3 or abs(d.ptr[1] - y) > 3):
                state(d)
            for _ in range(2):
                P.press(d, True); time.sleep(0.03); P.press(d, False); time.sleep(0.08)

        def zoom(title, app, want=None):
            n0 = len(d.g.seen)
            win = state(d)[0][title]
            dz = win['drag0']
            double_click(win['x'] + dz[0] + dz[2] // 2, win['y'] + dz[1] + dz[3] // 2)
            d.g.wait(b'zoomed by a double click', 20, start=n0)
            seen, end = [], time.monotonic() + 15
            while time.monotonic() < end:
                wins, ghosts, _ = state(d)
                mine = [g for g in ghosts if g['app'] == app]
                seen.extend(mine)
                now = wins.get(title)
                if now and not mine and frame_rect(now) != frame_rect(win) and \
                        (want is None or frame_rect(now) == want):
                    return frame_rect(win), frame_rect(now), seen
            raise AssertionError(f'{title} did not land after a double click: {seen[-3:]}')

        tap(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        until_back(d, 'Settings', 'settings.elf'); d.g.pump(1)
        d.g.command('perf slowmo 4', b'perf: slowmo', 60)   # a 300 ms flight, sampled
        was, filled, flight = zoom('Settings', 'settings.elf')
        assert filled[2] > was[2] + 100 and filled[3] >= was[3], f'a double click did not fill: {was} -> {filled}'
        assert flight and all(g['kind'] == 'zoom' for g in flight), f'expected a zoom in flight: {flight}'
        between = [g for g in flight if was[2] < g['w'] < filled[2]]
        assert between, f'it did not fly between the frames: {[(g["w"], g["h"]) for g in flight]}'
        print(f'zoom: double-clicked, it flew from {was} to {filled}, caught at {between[0]["w"]}x{between[0]["h"]}', flush=True)
        _, back, flight = zoom('Settings', 'settings.elf', want=was)
        assert back == was and any(filled[2] > g['w'] > was[2] for g in flight), \
            f'a second double click did not fly it back to {was}: {back}, {[(g["w"], g["h"]) for g in flight]}'
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        print(f'zoom: double-clicked again, back at {back}', flush=True)
        # photographed on the way, ten times slower
        d.g.command('perf slowmo 10', b'perf: slowmo', 60)
        n0 = len(d.g.seen)
        win = state(d)[0]['Settings']; dz = win['drag0']
        double_click(win['x'] + dz[0] + dz[2] // 2, win['y'] + dz[1] + dz[3] // 2)
        d.g.wait(b'zoomed by a double click', 20, start=n0)
        zooms, end = [], time.monotonic() + 30
        while len(zooms) < 5 and time.monotonic() < end:
            mine = [g for g in state(d)[1] if g['app'] == 'settings.elf' and g['kind'] == 'zoom']
            if mine and was[2] < mine[-1]['w'] < filled[2]:
                path = d.g.capture(out / f'zoom-{len(zooms)}.ppm')
                zooms.append(f"{path} (Settings at {mine[-1]['w']}x{mine[-1]['h']})")
                time.sleep(0.3)
            elif zooms and not mine:
                break
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        until_back(d, 'Settings', 'settings.elf', 30)
        zoom('Settings', 'settings.elf', want=was)          # and back, for what follows
        print('zoom captures:\n  ' + '\n  '.join(zooms), flush=True)

        # SLIDE: Settings back out on space 1, then the spaces changed under it
        tap(d, tile[0] + tile[2] // 2, tile[1] + tile[3] // 2)
        _, win = until_back(d, 'Settings', 'settings.elf')
        assert win and win['space'] == 0, f'Settings is not back on the first space: {win}'
        home = frame_rect(win)
        d.key('meta_l-2')                   # up a space: it leaves to the left
        flight, wins = settle(d, 1)
        out_ = [g for g in flight if g['app'] == 'settings.elf']
        assert out_ and all(g['kind'] == 'slide-out' for g in out_), f'going up a space, Settings did not slide out: {flight}'
        assert all(g['to'][1:] == home[1:] and g['to'][0] + g['to'][2] <= 0 for g in out_), \
            f'it was not aimed off the left of the screen: {out_[0]["to"]} from {home}'
        xs = [g['x'] for g in out_]
        assert xs == sorted(xs, reverse=True) and all(g['w'] == home[2] and g['alpha'] == 255 for g in out_), \
            f'it did not slide steadily left, whole: {[(g["x"], g["w"], g["alpha"]) for g in out_]}'
        assert frame_rect(wins['Settings']) == home and wins['Settings']['space'] == 0, \
            f'the slide moved the window itself: {wins["Settings"]} vs {home}'
        print(f'slide: up a space, Settings slid out left over {len(out_)} samples, x {xs[0]} -> {xs[-1]}', flush=True)
        d.key('meta_l-1')                   # down again: it comes back from the left
        flight, wins = settle(d, 0)
        in_ = [g for g in flight if g['app'] == 'settings.elf']
        assert in_ and all(g['kind'] == 'slide-in' and g['to'] == home for g in in_), \
            f'coming back, Settings did not slide in to {home}: {in_}'
        xs = [g['x'] for g in in_]
        assert xs == sorted(xs) and xs[0] < home[0], f'it did not come in from the left: {xs}'
        assert frame_rect(wins['Settings']) == home, f'Settings did not land where it was: {wins["Settings"]}'
        print(f'slide: down again, in from the left over {len(in_)} samples, landed at {home}', flush=True)
        # interrupted: up and straight back down -- it turns round mid-slide
        n0 = len(d.g.seen)
        d.key('meta_l-2')                   # (sends, then waits 0.2 s)
        d.key('meta_l-1')
        d.g.wait(b'workspace slide turned around in flight', 20, start=n0)
        _, wins = settle(d, 0)
        assert frame_rect(wins['Settings']) == home, \
            f'interrupted, Settings did not land where it was: {wins["Settings"]} vs {home}'
        print('slide: switched back mid-slide, it turned round and landed home', flush=True)
        # and photographed on the way, ten times slower
        d.g.command('perf slowmo 10', b'perf: slowmo', 60)
        d.key('meta_l-2')
        slides, end = [], time.monotonic() + 30
        while len(slides) < 6 and time.monotonic() < end:
            _, ghosts, _ = state(d)
            mine = [g for g in ghosts if g['app'] == 'settings.elf']
            if not mine:
                break
            path = d.g.capture(out / f'slide-{len(slides)}.ppm')
            slides.append(f"{path} (Settings at x {mine[-1]['x']})")
            time.sleep(0.5)
        d.g.command('perf slowmo 1', b'perf: slowmo', 60)
        d.key('meta_l-1'); settle(d, 0)
        print('slide captures:\n  ' + '\n  '.join(slides), flush=True)

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
          'mid-flight when interrupted, two windows move at once, a double click zooms a window '
          'and back on a spring, and changing workspace slides the windows, turning round when '
          'switched back mid-slide', flush=True)


if __name__ == '__main__':
    main()
