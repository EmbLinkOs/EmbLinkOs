#!/usr/bin/env python3
"""What composing the desktop costs, scenario by scenario.

Boots uefi-usb.img on a disposable copy, makes an account and logs in, then
drives the desktop through a fixed set of scenes over QMP and asks the
compositor after each one how many composes it did, of how many pixels, how
long they took and why (the serial `perf` command, kernel/gfx/comp_stats.c):

  idle      ten seconds of nothing happening
  pointer   the pointer swept across the desktop
  drag      the Settings window dragged by its title bar and back
  launcher  the Applications launcher opened and dismissed
  openclose a Terminal opened from the dock, then closed with GUI+W

Numbers under TCG are the emulator's, not a machine's: they compare one build
with another on the same host, never a build with a target. The counts --
composes, pixels, full-screen composes, their causes -- are the same anywhere.

--arch aarch64 boots the arm64 kernel and root filesystem instead (make
ARCH=aarch64), under HVF on an Apple Silicon host: the guest runs at the host
CPU's own speed, so those times are a real core's, composing on the CPU.
--accel tcg forces the emulator there too.

x86: build with FB_W=1280 FB_H=800; needs QEMU, OVMF (EMBLINK_OVMF_DIR) and GDB
(EMBLINK_GDB). Writes perf.json and a capture with the statistics overlay to
the output directory; --label names the run in the JSON.
"""
import argparse
import json
import re
import shutil
import tempfile
import time
from pathlib import Path

import os
import socket
import subprocess

from recovery_smoke import ROOT, Guest
from workspace_smoke import Desktop
from app_shot import Qmp
import shell_shot as S

W, H = 1280, 800

DOCK_Y = 745
DOCK_TERMINAL_X, DOCK_SETTINGS_X = 611, 668
SETTINGS_TITLE = (685, 62)          # the centred title: the window's drag zone


def perf(d):
    """The compositor's counters since the last reset, as numbers."""
    text = d.g.command('perf', b'perf: end', 60)
    out = {'src': {}}
    for line in text.splitlines():
        if not line.startswith('perf: ') or line.startswith('perf: end'):
            continue
        pairs = dict(kv.split('=', 1) for kv in line[6:].split() if '=' in kv)
        if 'src' in pairs:
            src = pairs.pop('src')
            out['src'][src] = {k: int(v) for k, v in pairs.items()}
        elif 'win' in pairs:
            win = pairs.pop('win')
            out.setdefault('windows', {})[win] = {k: int(v) for k, v in pairs.items()}
        else:
            for k, v in pairs.items():
                out[k] = v if 'x' in v else int(v)
    return out


def scene(d, results, name, body):
    d.g.command('perf reset', b'perf: reset', 60)
    t0 = time.monotonic()
    body()
    stats = perf(d)
    stats['wall_s'] = round(time.monotonic() - t0, 1)
    results[name] = stats
    span = max(stats['span_ms'], 1)
    print(f"{name:10} {stats['composes']:6} composes {stats['composes'] * 1000 / span:6.1f}/s "
          f"{stats['kpx'] / 1000:9.1f} Mpx {stats['full']:5} full  p50 {stats['p50_us'] / 1000:6.2f} ms "
          f"p95 {stats['p95_us'] / 1000:6.2f} ms  ptr p95 {stats['ptr_p95_us'] / 1000:6.2f} ms  "
          + (f"{stats.get('frames', 0)} frames beat p50 {stats.get('interval_p50_us', 0) / 1000:5.2f} "
             f"p95 {stats.get('interval_p95_us', 0) / 1000:5.2f} ms  " if 'frames' in stats else '')
          + ' '.join(f"{k}={v['composes']}" for k, v in stats['src'].items() if v['composes']),
          flush=True)


class Arm64Guest(Guest):
    """The arm64 machine, behind the same interface as recovery_smoke.Guest:
    QEMU virt with the kernel image and a disposable copy of its root
    filesystem, virtio GPU/keyboard/tablet, serial and QMP on Unix sockets.
    The tablet is ABSOLUTE, so the pointer goes where it is put (absolute)."""
    absolute = True

    def __init__(self, out, accel='hvf'):
        self.out = out; out.mkdir(); self.seen = bytearray(); self.menu = 1; self.selected = True
        self.graphics = 'virtio'
        disks = []
        for i, (src, iface) in enumerate([(ROOT / 'build/aarch64/embkfs-arm64.img', 'blk'),
                                          (ROOT / 'build/crash-seed.img', 'blk'),
                                          (ROOT / 'build/swap.img', 'blk'),
                                          (ROOT / 'build/nvme-scratch.img', 'nvme')]):
            copy = out / f'disk{i}.img'; shutil.copyfile(src, copy)
            disks += ['-drive', f'file={copy},format=raw,if=none,id=d{i}',
                      '-device', 'virtio-blk-pci,drive=d%d' % i if iface == 'blk' else f'nvme,serial=EMBKSCRATCH,drive=d{i}']
        machine = 'virt,gic-version=3' + (',accel=hvf' if accel == 'hvf' else '')
        cmd = ['qemu-system-aarch64', '-M', machine, '-cpu', 'host' if accel == 'hvf' else 'cortex-a72',
               '-smp', '4', '-m', '1024', *disks,
               '-device', f'virtio-gpu-pci,xres={W},yres={H}', '-device', 'virtio-keyboard-pci',
               '-device', 'virtio-tablet-pci', '-device', 'virtio-sound-pci,audiodev=a0', '-audiodev', 'none,id=a0',
               '-nic', 'none', '-display', 'none',
               '-serial', f'unix:{out / "serial.sock"},server=on,wait=off',
               '-qmp', f'unix:{out / "qmp.sock"},server=on,wait=off',
               '-kernel', str(ROOT / 'build/aarch64/kernel.img'), '-no-reboot']
        self.err = (out / 'qemu.log').open('wb'); self.log = (out / 'serial.log').open('wb')
        self.p = subprocess.Popen(cmd, cwd=ROOT, stdout=self.err, stderr=self.err)
        self.serial = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try: self.serial.connect(str(out / 'serial.sock')); break
            except (FileNotFoundError, ConnectionRefusedError): time.sleep(.1)
        else: self.close(); raise TimeoutError('serial socket missing')
        self.serial.settimeout(.1); self.qmp = Qmp(str(out / 'qmp.sock'))


def press(d, down):
    d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])


def put(d, x, y):
    """The absolute pointer to (x, y): QMP's axes run 0..0x7fff over the screen."""
    d.g.qmp.cmd('input-send-event', events=[
        {'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 0x7fff / (W - 1))}},
        {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 0x7fff / (H - 1))}}])
    S._pos[0], S._pos[1] = x, y


def move(d, x, y):
    if getattr(d.g, 'absolute', False): put(d, x, y); d.g.pump(0.4)
    else: S.move(d.g.qmp, x, y)


def click(d, x, y):
    move(d, x, y); S.click(d.g.qmp)


def glide(d, x, y, step=8, pause=0.03):
    """Small steps, as a hand moves -- not one jump."""
    while (S._pos[0], S._pos[1]) != (x, y):
        sx = max(-step, min(step, x - S._pos[0]))
        sy = max(-step, min(step, y - S._pos[1]))
        if getattr(d.g, 'absolute', False):
            put(d, S._pos[0] + sx, S._pos[1] + sy)
        else:
            d.g.qmp.cmd('input-send-event', events=[
                {'type': 'rel', 'data': {'axis': 'x', 'value': sx}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': sy}}])
            S._pos[0] += sx; S._pos[1] += sy
        d.g.pump(pause)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--label', default='run')
    ap.add_argument('--out')
    ap.add_argument('--arch', choices=('x86_64', 'aarch64'), default='x86_64')
    ap.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf', help='aarch64 only')
    args = ap.parse_args()
    out = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix='emblink-perf-'))
    out.mkdir(parents=True, exist_ok=True)
    print(f'Logs and results: {out}', flush=True)
    if args.arch == 'aarch64':
        d = Desktop.__new__(Desktop)
        d.out = out / 'guest'; d.g = Arm64Guest(d.out, args.accel); d.sock = d.out / 'gdb.sock'; d.symbols = {}
    else:
        disk = out / 'perf.img'
        shutil.copyfile(ROOT / 'uefi-usb.img', disk)
        d = Desktop(out / 'guest', disk)
    results = {}
    try:
        d.login(setup=True)
        S._pos[0] = None
        move(d, 640, 400)
        d.g.pump(5)

        scene(d, results, 'idle', lambda: d.g.pump(10))

        def pointer():
            for x, y in ((1100, 200), (200, 600), (1100, 600), (200, 200), (640, 400)):
                glide(d, x, y)
            d.g.pump(1)
        scene(d, results, 'pointer', pointer)

        click(d, DOCK_SETTINGS_X, DOCK_Y)
        d.g.wait(b'Settings: first frame presented'); d.g.pump(4)

        def drag():
            move(d, *SETTINGS_TITLE)
            press(d, True); d.g.pump(0.3)
            glide(d, SETTINGS_TITLE[0] - 160, SETTINGS_TITLE[1] + 120, step=6)
            glide(d, *SETTINGS_TITLE, step=6)
            press(d, False); d.g.pump(2)
        scene(d, results, 'drag', drag)

        def launcher():
            d.key('meta_l-spc'); d.g.pump(4)
            d.key('esc'); d.g.pump(3)
        scene(d, results, 'launcher', launcher)

        def openclose():
            start = len(d.g.seen)
            click(d, DOCK_TERMINAL_X, DOCK_Y)
            d.g.wait(b'Terminal: first frame presented', start=start); d.g.pump(3)
            d.key('meta_l-w'); d.g.pump(4)
        scene(d, results, 'openclose', openclose)

        d.g.command('perf hud', b'perf: hud on', 60)
        d.g.pump(3)
        results['capture'] = str(d.g.capture(out / 'perf-hud.ppm'))
    finally:
        d.g.close()
    results['label'] = args.label
    results['arch'] = args.arch + ('' if args.arch == 'x86_64' else '/' + args.accel)
    (out / 'perf.json').write_text(json.dumps(results, indent=2) + '\n')
    scenes = sum(1 for v in results.values() if isinstance(v, dict) and 'composes' in v)
    print(f'PASS: {scenes} scenes measured; {out / "perf.json"}', flush=True)


if __name__ == '__main__':
    main()
