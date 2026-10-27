#!/usr/bin/env python3
"""A USB mouse on the generic USB path -- UHCI and EHCI, not xHCI.

The xHCI driver has fed boot-protocol mice to the pointer for a long time;
the generic path the older controllers share found one and printed that it
had no driver. Boots the desktop with a USB mouse on a UHCI controller and
another on EHCI, and asserts each one moves the pointer by what it reports
and that its button reaches the desktop.

Needs QEMU, OVMF (EMBLINK_OVMF_DIR) and GDB (EMBLINK_GDB), like the other
desktop smokes; build with FB_W=1280 FB_H=800.
"""
import shutil
import tempfile
from pathlib import Path

from workspace_smoke import Desktop, ROOT


def pointer(d):
    text = d.g.command('perf windows', b'perf: end', 60)
    for line in text.splitlines():
        if line.startswith('perf: pointer '):
            pairs = dict(kv.split('=', 1) for kv in line[14:].split())
            return int(pairs['x']), int(pairs['y']), int(pairs['buttons'])
    raise AssertionError('no pointer line in perf windows')


def hmp(d, line):
    r = d.g.qmp.cmd('human-monitor-command', **{'command-line': line})
    return r.get('return', '') if isinstance(r, dict) else str(r)


def usb_mice(d):
    """QEMU's indexes of the USB mice (`info mice`: "QEMU HID Mouse"), in the
    order they were added -- UHCI's first, then EHCI's."""
    out = []
    for line in hmp(d, 'info mice').splitlines():
        if 'HID Mouse' in line and 'Mouse #' in line:
            out.append(int(line.split('Mouse #')[1].split(':')[0]))
    return sorted(out)


def rel(d, dx, dy):
    d.g.qmp.cmd('input-send-event', events=[
        {'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
        {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}])
    d.g.pump(0.5)


def button(d, down):
    d.g.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])
    d.g.pump(0.5)


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-usb-mouse-'))
    print(f'Logs: {out}', flush=True)
    disk = out / 'usb.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'guest', disk, extra=(
        '-device', 'piix3-usb-uhci,id=uhci', '-device', 'usb-mouse,bus=uhci.0,id=umouse',
        '-device', 'usb-ehci,id=ehci', '-device', 'usb-mouse,bus=ehci.0,id=emouse'))
    try:
        d.login(setup=True)
        d.g.pump(2)
        ready = d.g.seen.count(b'USB HID: boot mouse ready')
        assert ready >= 2, f'expected two boot mice ready, the log says {ready}'
        assert b'no driver' not in d.g.seen, 'a USB mouse was found with no driver'
        mice = usb_mice(d)
        assert len(mice) == 2, f'QEMU lists {len(mice)} USB mice: {hmp(d, "info mice")}'
        for name, index in zip(('UHCI', 'EHCI'), mice):
            hmp(d, f'mouse_set {index}')          # this one gets the events
            x0, y0, _ = pointer(d)
            # small steps, so a pointer curve (if one is on) stays near 1:1
            for _ in range(5):
                rel(d, 10, -6)
            x1, y1, _ = pointer(d)
            assert x1 - x0 >= 40 and y0 - y1 >= 24, f'{name}: the pointer did not follow: {(x0, y0)} -> {(x1, y1)}'
            button(d, True)
            _, _, b = pointer(d)
            button(d, False)
            _, _, b2 = pointer(d)
            assert b & 1 and not b2 & 1, f'{name}: the left button did not go down and up: {b} {b2}'
            print(f'{name} mouse: pointer {(x0, y0)} -> {(x1, y1)}, left button down and up', flush=True)

        # THE CURVE (the launcher boots the pointer 1:1; this turns it on):
        # an aim stays 1:1, a burst goes further than it counted
        d.g.command('pointer accel', b'pointer: accelerated', 60)
        hmp(d, f'mouse_set {mice[0]}')
        P0 = pointer(d)
        for _ in range(8):
            d.g.qmp.cmd('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': -4}}])
            d.g.pump(0.25)
        P1 = pointer(d)
        assert P0[0] - P1[0] == 32, f'a slow aim of 32 counts moved {P0[0] - P1[0]} px'
        for _ in range(12):
            d.g.qmp.cmd('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': 25}}])
        d.g.pump(1)
        P2 = pointer(d)
        assert P2[0] - P1[0] > 300 * 1.3, f'a burst of 300 counts moved only {P2[0] - P1[0]} px'
        print(f'accelerated: a slow aim of 32 counts moved {P0[0] - P1[0]} px, a burst of 300 moved {P2[0] - P1[0]} px', flush=True)
    finally:
        d.g.close()
    print('PASS: USB boot mice on UHCI and EHCI move the pointer and press its button; '
          'accelerated, an aim stays 1:1 and a burst goes further', flush=True)


if __name__ == '__main__':
    main()
