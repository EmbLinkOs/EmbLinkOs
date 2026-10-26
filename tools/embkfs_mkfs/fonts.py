"""The fonts on the image, one list for both image builders.

mkfs_embkfs.py (x86) and mkfs_arm64.py both install exactly these, so the two
images cannot carry different faces. DejaVu comes from the HOST's install
(mkfs_embkfs._read_font knows where to look); Terminal's MesloLGS Nerd Font
Mono is vendored in the tree (third_party/fonts/meslo-nf), so every build gets
the same four faces whatever the host has. Licences for both are in
system/licenses, which both builders install too.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (DejaVu file on the host, name under /system/fonts)
HOST_FACES = [
    ("DejaVuSans.ttf", "font.ttf"),                  # the UI face
    ("DejaVuSans-Bold.ttf", "bold.ttf"),
    ("DejaVuSansCondensed.ttf", "condensed.ttf"),
    ("DejaVuSansCondensed-Bold.ttf", "condensed-bold.ttf"),
    ("DejaVuSerif.ttf", "serif.ttf"),
    ("DejaVuSerif-Bold.ttf", "serif-bold.ttf"),
    ("DejaVuSansMono.ttf", "mono.ttf"),              # fixed pitch: tables align
    ("DejaVuSansMono-Bold.ttf", "mono-bold.ttf"),
]

# (file in the tree, name under /system/fonts): Terminal's faces
TREE_FACES = [
    ("third_party/fonts/meslo-nf/MesloLGSNerdFontMono-Regular.ttf", "meslo-nf-mono.ttf"),
    ("third_party/fonts/meslo-nf/MesloLGSNerdFontMono-Bold.ttf", "meslo-nf-mono-bold.ttf"),
    ("third_party/fonts/meslo-nf/MesloLGSNerdFontMono-Italic.ttf", "meslo-nf-mono-italic.ttf"),
    ("third_party/fonts/meslo-nf/MesloLGSNerdFontMono-BoldItalic.ttf", "meslo-nf-mono-bold-italic.ttf"),
]


def font_files(read_host_font):
    """[(image path bytes, data)] for every face found. A missing host face is
    reported by read_host_font; a missing tree face is a broken checkout and
    stops the build."""
    out = []
    for source, name in HOST_FACES:
        data = read_host_font(source)
        if data is not None:
            out.append((("system/fonts/" + name).encode(), data))
    for source, name in TREE_FACES:
        with open(os.path.join(ROOT, source), "rb") as fh:
            out.append((("system/fonts/" + name).encode(), fh.read()))
    return out


def licence_files():
    """[(image path bytes, data)] for system/licenses, as the tree has it."""
    out = []
    directory = os.path.join(ROOT, "system", "licenses")
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, "rb") as fh:
                out.append((("system/licenses/" + name).encode(), fh.read()))
    return out
