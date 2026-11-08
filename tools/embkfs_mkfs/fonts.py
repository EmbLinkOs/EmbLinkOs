"""The fonts on the image, one list for both image builders.

mkfs_embkfs.py (x86) and mkfs_arm64.py both install exactly these, so the two
images cannot carry different faces. The interface's face, Inter, and
Terminal's MesloLGS Nerd Font Mono are vendored in the tree
(third_party/fonts/inter, third_party/fonts/meslo-nf), so every build gets the
same faces whatever the host has; DejaVu -- the fallback for what Inter does
not cover, and the condensed, serif and mono faces -- comes from the HOST's
install (mkfs_embkfs._read_font knows where to look). Licences for all three
are in system/licenses, which both builders install too.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (DejaVu file on the host, name under /system/fonts)
HOST_FACES = [
    ("DejaVuSans.ttf", "fallback.ttf"),              # what Inter lacks (font.h: THE FALLBACK)
    ("DejaVuSansCondensed.ttf", "condensed.ttf"),
    ("DejaVuSansCondensed-Bold.ttf", "condensed-bold.ttf"),
    ("DejaVuSerif.ttf", "serif.ttf"),
    ("DejaVuSerif-Bold.ttf", "serif-bold.ttf"),
    ("DejaVuSansMono.ttf", "mono.ttf"),              # fixed pitch: tables align
    ("DejaVuSansMono-Bold.ttf", "mono-bold.ttf"),
]

# (file in the tree, name under /system/fonts): the UI's faces, Terminal's
TREE_FACES = [
    ("third_party/fonts/inter/Inter-Regular.ttf", "font.ttf"),      # the UI face
    ("third_party/fonts/inter/Inter-SemiBold.ttf", "bold.ttf"),     # its emphasis
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
