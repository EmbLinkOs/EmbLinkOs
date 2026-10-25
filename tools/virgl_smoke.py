#!/usr/bin/env python3
"""The desktop composed on the host's GPU (virgl), against the same desktop
composed on the CPU.

Boots uefi-usb.img twice on QEMU's virtio-vga-gl -- the guest negotiates
VIRTIO_GPU_F_VIRGL and the compositor draws through virglrenderer -- once as
it comes, once with opt/emblink/compose=0 so the CPU composes on the very
same device. Both runs go through the same scenes with the same input: the
desktop after setup and login, a terminal being typed into (window images
updated where they changed), the Windows demo's two kernel-chromed windows
started from it (title bars, rings, rounded corners, focus shadows), one of
them dragged, Settings > Displays, whose Rendering row must name what
composes, and a resolution trial there and its revert (a new surface and
scanout resource under the GPU, and back).

Then every GPU frame is compared with its CPU twin: the two compositors
draw the same layers, so they may differ only along the edges each
antialiases its own way and where the GPU's shadow deliberately reaches
under a window's bottom edge. The budget for that is printed with each
comparison.

Linux only, with QEMU's virgl and a DRM render node (vgem is enough), and
vncdotool (frames of a GL scanout are read over VNC). On an ARM host point
EMBLINK_GDB at gdb-multiarch and EMBLINK_NM at the host's nm. Run with the
Python that has vncdotool's Pillow:
  EMBLINK_GDB=gdb-multiarch EMBLINK_NM=nm ~/vncenv/bin/python3 tools/virgl_smoke.py
Build uefi-usb.img first; never rebuild binaries while the guests run.
"""
import json, os, shutil, sys, tempfile, time
from pathlib import Path
from PIL import Image, ImageChops
from recovery_smoke import ROOT
from workspace_smoke import Desktop
from native_ui import NativeUI

CLOCK_W, BAR_H = 280, 34           # the top bar's clock, at its right end: the minute differs between runs


def run(out, cpu):
    name = 'cpu' if cpu else 'gpu'
    disk = out / f'{name}.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / name, disk, graphics='virgl', cpu_compose=cpu)
    shots = {}
    try:
        d.login(setup=True)
        log = bytes(d.g.seen)
        if cpu:
            assert b'virtio-gpu: composition kept on the CPU' in log, 'the CPU run did not keep composition on the CPU'
            assert b'compositor: composing on the' not in log
        else:
            assert b"virtio-gpu: composing on the host's GPU (virgl), probe frame correct" in log, \
                'the GPU path did not come up (see serial.log)'
            assert b'compositor: composing on the host GPU (virgl)' in log

        def settle(seconds=2.5):
            d.g.pump(seconds)

        def animated():
            # The Windows demo's two windows draw live meters: their insides
            # never hold still, so the settling below looks past them.
            rects = []
            for w in d.state()['windows']:
                if w['visible'] and w['title'] in ('System Monitor', 'Activity'):
                    rects.append((w['x'], w['y'] + 34, w['x'] + w['cw'], w['y'] + 34 + w['ch']))
            return rects

        def shot(scene):
            # A frame the screen has settled on: two captures two seconds apart
            # that agree (the clock and live meters aside). Apps redraw after a
            # change at their own pace, and the CPU-composed run is the slower
            # of the two; a path that left stale pixels would still fail the
            # comparison, because a stale frame is a stable one.
            settle()
            path = d.g.capture(out / f'{name}-{scene}.png')
            for _ in range(15):
                d.g.pump(2)
                again = d.g.capture(out / f'{name}-{scene}-again.png')
                live = animated()
                if not ImageChops.difference(masked(path, live), masked(again, live)).getbbox():
                    break
                os.replace(again, path)
            else:
                raise AssertionError(f'{scene}: the screen never settled')
            os.remove(again)
            shots[scene] = path
            print(f'Capture: {name}-{scene}', flush=True)

        # 1. The desktop after setup and login: wallpaper window, top bar, dock.
        d.click(640, 400)
        shot('desktop')

        # 2. A terminal typed into: only its changed rows go to the GPU.
        term = d.launch('Terminal', 'Terminal')
        d.click(term['x'] + term['cw'] // 2, term['y'] + term['ch'] // 2)
        before = d.g.capture(out / f'{name}-terminal-before.png')
        d.type('echo composed\n')
        shot('terminal')
        t = d.window('Terminal')
        box = (t['x'] + 4, t['y'] + 40, t['x'] + t['cw'] - 4, t['y'] + t['ch'] - 4)
        changed = ImageChops.difference(Image.open(before).convert('RGB').crop(box),
                                        Image.open(shots['terminal']).convert('RGB').crop(box)).getbbox()
        assert changed, 'typing into the terminal changed nothing on screen'

        # 3. Two windows with kernel chrome (the Windows demo, started from the
        #    terminal), one in front of the other.
        d.type('wmdemo\n')
        d.wait_window('System Monitor'); d.wait_window('Activity')
        d.g.pump(3)
        shot('windows')

        # 4. The front one dragged by its title bar: its old place, its new
        #    place and its shadow are all repainted.
        a = d.window('Activity')
        gx, gy = a['x'] + a['cw'] // 2, a['y'] + 17
        d.click(gx, gy)
        d.g.qmp.cmd('human-monitor-command', **{'command-line': 'mouse_button 1'}); d.g.pump(.4)
        for _ in range(6):
            d.g.qmp.cmd('human-monitor-command', **{'command-line': 'mouse_move -30 25'}); d.g.pump(.15)
        d.g.qmp.cmd('human-monitor-command', **{'command-line': 'mouse_button 0'}); d.g.pump(1)
        moved = d.window('Activity')
        assert (moved['x'], moved['y']) != (a['x'], a['y']), 'the Activity window did not move'
        shot('moved')

        # 5. Settings > Displays names what composes.
        s = d.launch('Settings', 'Settings')
        ui = NativeUI(d); ui.click_text(s, 'Displays'); d.g.pump(1.5)
        labels = [r['text'] for r in NativeUI(d).labels(d.window('Settings'))]
        want = 'Software' if cpu else 'host GPU (virgl)'
        assert 'Rendering' in labels and want in labels, f'Rendering row does not say {want!r}: {labels}'
        shot('settings')

        # 6. A resolution trial and its revert: a new surface, a new scanout
        #    resource attached to the 3D context, the canvas made again over
        #    the new backbuffer -- and back.
        def monitor():
            return d.debug('''import gdb,json
s=gdb.parse_and_eval("'kernel/display/control.c'::state")
f=gdb.parse_and_eval("'kernel/drivers/video/framebuffer.c'::fb")
print('RESULT '+json.dumps({'phase':int(s['phase']),'width':int(f['width']),'height':int(f['height'])}))''')
        def until(test, what, seconds=30):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if test(): return
                d.g.pump(.4)
            raise AssertionError(what + '; ' + json.dumps(monitor()))
        def reflowed(w, h):
            # the desktop window has the new size and has presented all of it
            rows = d.state()['windows']
            desk = next(x for x in rows if x['desktop'])
            return desk['cw'] == w and desk['ch'] == h and \
                int.from_bytes(d.read('home', desk['pid'], 'g_full_present', 4), 'little') == 0
        settings = d.window('Settings'); ui = NativeUI(d)
        ui.reveal(settings, 'Resolution')
        current = next(r for r in ui.labels(settings) if ' × ' in r['text'] and not r['text'].endswith('mm'))['text']
        ui.click_text(settings, current); ui.reveal(settings, '1024 × 768'); ui.click_text(settings, '1024 × 768')
        ui.reveal(settings, 'Apply Resolution'); ui.click_text(settings, 'Apply Resolution')
        until(lambda: monitor()['phase'] == 3, 'the resolution trial did not start')
        until(lambda: (monitor()['width'], monitor()['height']) == (1024, 768) and reflowed(1024, 768), 'no 1024 x 768 desktop')
        shot('trial-1024')
        ui = NativeUI(d); ui.click_text(d.window('Settings'), 'Revert')
        until(lambda: (monitor()['width'], monitor()['height']) == (1280, 800) and monitor()['phase'] == 0 and
              reflowed(1280, 800), 'the revert did not bring 1280 x 800 back')
        shot('reverted')

        log = bytes(d.g.seen)
        assert b'GPU composition stopped' not in log, 'the GPU path fell back to the CPU during the run'
    finally:
        d.g.close()
    return shots


def masked(path, extra=()):
    img = Image.open(path).convert('RGB')
    img.paste((0, 0, 0), (img.size[0] - CLOCK_W, 0, img.size[0], BAR_H))
    for r in extra:
        box = (max(0, r[0]), max(0, r[1]), min(img.size[0], r[2]), min(img.size[1], r[3]))
        if box[2] > box[0] and box[3] > box[1]:
            img.paste((0, 0, 0), box)
    return img


def compare(scene, gpu, cpu, budget):
    a = masked(gpu); b = masked(cpu)
    assert a.size == b.size, (a.size, b.size)
    diff = ImageChops.difference(a, b).convert('L')
    hist = diff.histogram()
    total = a.size[0] * a.size[1]
    far = sum(hist[41:])                          # visibly different: > 40 of 255 in the brightest channel
    mean = sum(i * n for i, n in enumerate(hist)) / total
    print(f'{scene}: {far} pixels ({100 * far / total:.3f}%) differ by more than 40, mean difference {mean:.3f}'
          f' (budget {100 * budget:.2f}%)', flush=True)
    diff.point(lambda v: 255 if v > 40 else 0).save(str(gpu).replace('.png', '-diff.png'))
    assert far <= budget * total, f'{scene}: the GPU frame differs from the CPU frame beyond the edges'
    assert mean < 1.5, f'{scene}: the GPU frame differs from the CPU frame overall'


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-virgl-'))
    print('Logs and captures: ' + str(out), flush=True)
    gpu = run(out, cpu=False)
    cpu = run(out, cpu=True)
    # Window edges: arcs, rings and the shadow band under a window are drawn
    # by each compositor its own way; everything else must match.
    for scene, budget in (('desktop', .002), ('windows', .02), ('moved', .02), ('terminal', .03), ('settings', .03),
                          ('trial-1024', .03), ('reverted', .03)):
        compare(scene, gpu[scene], cpu[scene], budget)
    print('virgl smoke: the host GPU composed the desktop, windows, a drag, typing, Settings and a resolution '
          'change and back, frame for frame with the CPU compositor', flush=True)


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    main()
