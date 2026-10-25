#!/usr/bin/env python3
"""The native shell beyond its builtins, photographed in the Terminal.

Boots uefi-usb.img (virtio-vga, a disposable NVMe disk), sets up an account,
logs in, opens the Terminal and types, the way a person would:

  1. text programs in pipelines -- sh's output into `lines`, values piped
     into sh as text, flags like -c;
  2. redirections -- out>, out>>, err>, in< on a program and on builtins;
  3. scripts -- a native script and an sh script run by their #! lines, with
     arguments, and `source` loading a definition;
  4. which kind a command is, and an argument limit failing loudly;
  5. an interactive sh session started from the native prompt.

Each scene is cleared first and captured when the screen has settled. The
checks live in `test shellio` (kernel/selftests.c, run by
tools/console_test.py); this is what it looks like.

    python3 tools/shell_io_shot.py
Build uefi-usb.img first; never rebuild binaries while the guest runs.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from recovery_smoke import ROOT
from workspace_smoke import Desktop

SCENES = [
    ("text-pipes", [
        "sh -c \"printf 'alpha\\nbeta\\ngamma\\n'\" | lines | reverse",
        "range 4 | sh -c \"while read n; do echo item-$n; done\" | lines | count",
        "ls /system | sh -c cat | lines | count",
    ]),
    ("redirections", [
        "sysinfo out> sysinfo.txt",
        "sh -c \"echo one; echo two >&2\" out> both.txt err> errors.txt",
        "cat both.txt; cat errors.txt",
        "sh -c \"echo three\" out>> both.txt; lines in< both.txt",
        "range 3 out> numbers.txt; cat numbers.txt | lines | count",
    ]),
    ("scripts", [
        "echo \"#!/system/bin/shell.elf\\necho $args | join \\\" and \\\"\\n\" | save both.esh",
        "./both.esh this that",
        "echo \"#!/system/bin/sh\\necho hello from sh, $1\\n\" | save hi.sh; ./hi.sh EmbLink",
        "echo \"def double(x) { echo ($x * 2) }\\n\" | save lib.esh; source lib.esh; double 21",
    ]),
    ("which-limits", [
        "which sh",
        "which sysinfo",
        "sh -c 'echo $#' sh $(range 200)",
    ]),
    ("interactive-sh", [
        "sh",
        "echo inside the POSIX shell: $0",
        "for x in 1 2 3; do printf '%s ' $x; done; echo",
        "exit",
        "echo \"back at the native prompt\"",
    ]),
]


def main():
    out = Path(tempfile.mkdtemp(prefix='emblink-shellio-'))
    print('Logs and captures: ' + str(out), flush=True)
    disk = out / 'shell.img'
    shutil.copyfile(ROOT / 'uefi-usb.img', disk)
    d = Desktop(out / 'boot', disk)
    try:
        d.login(setup=True)
        term = d.launch('Terminal', 'Terminal')
        d.click(term['x'] + term['cw'] // 2, term['y'] + term['ch'] // 2)
        for name, lines in SCENES:
            d.type('clear\n'); d.g.pump(2)
            for line in lines:
                d.type(line + '\n')
                d.g.pump(4)
            d.g.pump(3)
            ppm = d.snap('shell-' + name)
            png = ppm.with_suffix('.png')
            subprocess.run([sys.executable, str(ROOT / 'tools/ppm2png.py'), str(ppm), str(png)], check=True)
            print('Capture: ' + str(png), flush=True)
        log = bytes(d.g.seen)
        assert b'PANIC' not in log
    finally:
        d.g.close()


if __name__ == '__main__':
    main()
