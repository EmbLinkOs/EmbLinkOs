#!/usr/bin/env python3
"""Open a Terminal and print what its probe says: a diagnostic, not a test.

    python3 tools/terminal_probe_dump.py ["command typed first" ...]
Boots uefi-usb.img on a disposable disk, logs in, opens the Terminal, types
each argument followed by Enter, then prints the probe's state and screen.
"""
import json, shutil, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from recovery_smoke import ROOT
from workspace_smoke import Desktop
from native_terminal import TerminalState


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-probe-'))
    disk = out / 'probe.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'boot', disk)
    try:
        d.login(setup=True)
        d.g.qmp.cmd('human-monitor-command', **{'command-line': f'gdbserver unix:{d.sock},server=on,wait=off'})
        screen = TerminalState(d)
        w = d.launch('Terminal', 'Terminal')
        d.g.pump(4)
        for line in sys.argv[1:]:
            d.type(line + '\n')
            d.g.pump(3)
        p = screen.state(w)
        p.pop('cells', None)
        print(json.dumps(p, indent=1, ensure_ascii=False))
        d.snap('probe-dump')
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
