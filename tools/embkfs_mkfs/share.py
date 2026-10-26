"""/system/share on the image, one list for both image builders.

mkfs_embkfs.py (x86) and mkfs_arm64.py both install exactly these.

    /system/share/powerlevel10k/p10k-<style>.zsh
        the configurations `p10k configure` starts from, which the shell's
        `prompt style NAME` loads (docs/SHELL.md, "The prompt"); MIT, licence
        in system/licenses/powerlevel10k-MIT.txt
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

P10K_STYLES = ("lean", "lean-8colors", "classic", "rainbow", "pure")


def share_files():
    """[(image path bytes, data)]. A missing file is a broken checkout and
    stops the build."""
    out = []
    for style in P10K_STYLES:
        name = f"p10k-{style}.zsh"
        with open(os.path.join(ROOT, "third_party", "powerlevel10k", "config", name), "rb") as fh:
            out.append((("system/share/powerlevel10k/" + name).encode(), fh.read()))
    return out
