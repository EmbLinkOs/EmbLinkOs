"""One EmbLinkOS QEMU at a time -- and only EmbLinkOS's QEMUs count.

Every tool here refuses to start a guest while another is running, because two
of them at once share one accelerator and one set of disk images, and the
result is a test that fails for a reason that has nothing to do with the
change under test. That rule is right. Its old implementation was not quite:
`pgrep -x qemu-system-x86_64` counts EVERY such process on the machine, and a
different project running its own test suite in another window -- QEMUs that
share nothing with this tree -- kept every tool in this repository refusing for
as long as that suite ran. Twenty-seven tools, one false positive.

A QEMU is ours if its working directory is inside this repository: the
Makefile runs every tool from the tree root, and nothing here starts a guest
from anywhere else. A process whose directory cannot be read is treated as
ours -- refusing on a doubt is the failure this guard exists to make.
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXES = ("qemu-system-x86_64", "qemu-system-aarch64")


def _cwd_of(pid):
    """The process's working directory, or '' if it could not be read."""
    try:
        r = subprocess.run(["lsof", "-a", "-p", pid, "-d", "cwd", "-Fn"],
                           capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    for line in r.stdout.splitlines():
        if line.startswith("n"):
            return line[1:]
    return ""


def other_qemu_running():
    """True if a QEMU belonging to THIS repository is running."""
    for exe in EXES:
        r = subprocess.run(["pgrep", "-x", exe], capture_output=True, text=True)
        for pid in r.stdout.split():
            cwd = _cwd_of(pid)
            if not cwd or cwd == ROOT or cwd.startswith(ROOT + os.sep):
                return True
    return False
