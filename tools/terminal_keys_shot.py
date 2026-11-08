#!/usr/bin/env python3
"""Key identity, photographed in the Terminal.

Boots uefi-usb.img (virtio-vga, a disposable NVMe disk), logs in, opens the
Terminal and runs `ttyprobe --identity`, pressing the keys the way a person
would over QMP: F5, Ctrl+Up, Shift+F3, Alt+x, Insert, Ctrl+Left, Backspace,
Ctrl+Backspace, then Num Lock and the keypad in application mode. The probe
prints what the terminal sent for each, so the capture is the evidence; the
same run is asserted by tools/terminal_smoke.py.

    python3 tools/terminal_keys_shot.py
Build uefi-usb.img first; never rebuild binaries while the guest runs.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from recovery_smoke import ROOT
from workspace_smoke import Desktop


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-keys-'))
    print('Logs and captures: ' + str(out), flush=True)
    disk = out / 'keys.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'boot', disk)
    try:
        d.login(setup=True)
        term = d.launch('Terminal', 'Terminal')
        d.click(term['x'] + term['cw'] // 2, term['y'] + term['ch'] // 2)
        d.hover(term['x'] + term['cw'] + 60, term['y'] + 40)       # the pointer out of the text
        d.type('/data/apps/ttyprobe/ttyprobe.elf --identity\n'); d.g.pump(3)
        for key in ['f5', 'ctrl-up', 'shift-f3', 'alt-x', 'insert', 'ctrl-left', 'backspace', 'ctrl-backspace']:
            d.key(key)
        d.g.pump(2)
        for key in ['num_lock', 'kp_5', 'kp_enter', 'num_lock']:
            d.key(key)
        d.g.pump(4)
        ppm = d.snap('terminal-key-identity')
        png = ppm.with_suffix('.png')
        subprocess.run([sys.executable, str(ROOT / 'tools/ppm2png.py'), str(ppm), str(png)], check=True)
        print('Capture: ' + str(png), flush=True)
        assert b'PANIC' not in bytes(d.g.seen)
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
