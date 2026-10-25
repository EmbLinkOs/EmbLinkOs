#!/usr/bin/env python3
"""The shell's own line editor, photographed in the Terminal.

Boots uefi-usb.img (virtio-vga, a disposable NVMe disk), logs in, opens the
Terminal and types the way a person would, over QMP:

  1. completion -- Tab on a word several commands share opens the menu, Tab
     again chooses;
  2. history search -- Ctrl+R and a few letters find an earlier command;
  3. a multi-line program -- Enter on an open block adds a line instead of
     failing, and the closed block runs;
  4. history that outlives the window -- the Terminal is closed and a new one
     opened, and its shell recalls what the first one ran.

The behaviour is asserted by shell/test/edit_test.c and complete_test.c on the
host and by tools/terminal_smoke.py on the OS; this is what it looks like.

    python3 tools/shell_edit_shot.py
Build uefi-usb.img first; never rebuild binaries while the guest runs.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from recovery_smoke import ROOT
from workspace_smoke import Desktop


def shot(d, name):
    ppm = d.snap(name)
    png = ppm.with_suffix('.png')
    subprocess.run([sys.executable, str(ROOT / 'tools/ppm2png.py'), str(ppm), str(png)], check=True)
    print('Capture: ' + str(png), flush=True)


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-edit-'))
    print('Logs and captures: ' + str(out), flush=True)
    disk = out / 'edit.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'boot', disk)
    try:
        d.login(setup=True)
        term = d.launch('Terminal', 'Terminal')
        d.click(term['x'] + term['cw'] // 2, term['y'] + term['ch'] // 2)
        d.hover(term['x'] + term['cw'] + 60, term['y'] + 40)
        for line in ['echo "the shell edits its own line"', 'date', 'ls /system']:
            d.type(line + '\n'); d.g.pump(3)
        d.type('clear\n'); d.g.pump(2)

        d.type('s'); d.key('tab'); d.g.pump(2); d.key('tab'); d.g.pump(2)
        shot(d, 'shell-edit-completion')
        d.key('ctrl-c'); d.g.pump(1)

        d.key('ctrl-r'); d.type('sys'); d.g.pump(2)
        shot(d, 'shell-edit-search')
        d.key('ctrl-g'); d.key('ctrl-c'); d.type('clear\n'); d.g.pump(2)

        d.type('for n in $(range 3) {\n'); d.g.pump(1)
        d.type('echo ($n * 10)\n'); d.g.pump(1)
        d.type('}\n'); d.g.pump(3)
        shot(d, 'shell-edit-multiline')

        # Close this Terminal and open another from the launcher: a new shell,
        # in the same session, reading the history file the first one wrote.
        d.key('meta_l-w'); d.g.pump(3)
        term = d.launch('Terminal', 'Terminal')
        d.click(term['x'] + term['cw'] // 2, term['y'] + term['ch'] // 2)
        d.hover(term['x'] + term['cw'] + 60, term['y'] + 40)
        d.type('history | last 5\n'); d.g.pump(3)
        d.key('up'); d.key('up'); d.g.pump(2)
        shot(d, 'shell-edit-history')
        assert b'PANIC' not in bytes(d.g.seen)
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
