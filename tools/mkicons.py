#!/usr/bin/env python3
"""Build .eic multi-resolution icon containers from real art.

    icons/masters/<name>.png  ->  system/images/<name>.eic

One master in, every size the shell asks for out. See docs/ICONS.md for the
container layout and the reasoning; the short version is that .eic stores
pixels in exactly the premultiplied BGRA the renderer already uses, so the OS
loads an icon with a header parse and a pointer -- no decode, no conversion.

Until an icon has a PNG master we fall back to the legacy system/images/<name>.pam
so the desktop keeps working (those are capped by their 96x96 source).
"""

import argparse
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTERS = os.path.join(REPO, "icons", "masters")
OUTDIR = os.path.join(REPO, "system", "images")

# Chosen so every size the shell currently requests (40 dock, 44 ghost -> 48,
# 56 grid, 64 desktop) lands on an exact level and needs no resampling.
DEFAULT_LADDER = [16, 24, 32, 40, 48, 56, 64, 96, 128]

MAGIC = b"EICO"
VERSION = 1
FLAG_PREMULTIPLIED = 1
HEADER_SIZE = 32
LEVEL_ENTRY_SIZE = 16
ALIGN = 16


def _need_pillow():
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        sys.exit("mkicons: Pillow is required to read PNG masters "
                 "(pip install Pillow)")
    from PIL import Image
    return Image


def read_pam(path):
    """Minimal P7 RGB_ALPHA / RGB reader -> (w, h, RGBA bytes).

    Only used for the bootstrap path, so it accepts exactly the shape the
    in-tree .pam files have rather than the whole PAM spec.
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P7"):
        raise ValueError(f"{path}: not a PAM file")
    end = data.find(b"ENDHDR\n")
    if end < 0:
        raise ValueError(f"{path}: no ENDHDR")
    hdr = data[:end].decode("ascii", "replace")
    fields = {}
    for line in hdr.splitlines():
        parts = line.split()
        if len(parts) == 2:
            fields[parts[0]] = parts[1]
    w = int(fields["WIDTH"])
    h = int(fields["HEIGHT"])
    depth = int(fields["DEPTH"])
    if int(fields.get("MAXVAL", 255)) != 255 or depth not in (3, 4):
        raise ValueError(f"{path}: unsupported PAM (depth={depth})")
    px = data[end + 7:]
    if len(px) < w * h * depth:
        raise ValueError(f"{path}: truncated pixel data")
    if depth == 4:
        return w, h, px[:w * h * 4]
    out = bytearray()
    for i in range(w * h):
        out += px[i * 3:i * 3 + 3] + b"\xff"
    return w, h, bytes(out)


def load_master(name, Image):
    """Return (PIL RGBA image, source description, is_bootstrap)."""
    png = os.path.join(MASTERS, name + ".png")
    if os.path.exists(png):
        return Image.open(png).convert("RGBA"), os.path.relpath(png, REPO), False
    pam = os.path.join(OUTDIR, name + ".pam")
    if os.path.exists(pam):
        w, h, rgba = read_pam(pam)
        return (Image.frombytes("RGBA", (w, h), rgba),
                os.path.relpath(pam, REPO), True)
    raise FileNotFoundError(f"no master for icon '{name}'")


def square(img, Image):
    """Pad (never stretch) a non-square master onto a transparent canvas."""
    w, h = img.size
    if w == h:
        return img
    side = max(w, h)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - w) // 2, (side - h) // 2))
    return canvas


def premultiply(img, Image):
    """Straight RGBA -> premultiplied RGBA.

    Resizing must happen in premultiplied space, otherwise the colour of fully
    transparent pixels bleeds into visible neighbours (halos on soft edges).
    """
    r, g, b, a = img.split()
    from PIL import ImageChops
    return Image.merge("RGBA", (ImageChops.multiply(r, a),
                                ImageChops.multiply(g, a),
                                ImageChops.multiply(b, a),
                                a))


def level_bytes(img, size, Image):
    """Resize a premultiplied master to size x size and pack it for the OS."""
    lv = img.resize((size, size), Image.LANCZOS)
    px = lv.load()
    out = bytearray(size * size * 4)
    i = 0
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            # LANCZOS overshoots at hard edges and can push a channel above its
            # alpha -- impossible for a premultiplied pixel, and visible as a
            # bright fringe. Clamp back into the valid range.
            if r > a:
                r = a
            if g > a:
                g = a
            if b > a:
                b = a
            # (a<<24)|(r<<16)|(g<<8)|b little-endian == B,G,R,A in memory,
            # which is EMBK_PIXFMT_BGRA8888_PRE -- what the renderer blits.
            out[i] = b
            out[i + 1] = g
            out[i + 2] = r
            out[i + 3] = a
            i += 4
    return bytes(out)


def build_icon(name, ladder, Image, quiet=False):
    img, src, bootstrap = load_master(name, Image)
    img = square(img, Image)
    master_side = img.size[0]

    levels = [s for s in ladder if s > 0]
    biggest = max(levels)
    if master_side < biggest:
        print(f"mkicons: WARNING: {name}: master is {master_side}px but the "
              f"ladder goes to {biggest}px -- levels above {master_side} are "
              f"upscaled and will look soft. Supply a larger "
              f"icons/masters/{name}.png.", file=sys.stderr)

    pm = premultiply(img, Image)

    payloads = [(s, level_bytes(pm, s, Image)) for s in levels]

    table_size = LEVEL_ENTRY_SIZE * len(payloads)
    offset = HEADER_SIZE + table_size
    offset = (offset + ALIGN - 1) & ~(ALIGN - 1)

    table = bytearray()
    blobs = bytearray()
    cursor = offset
    for size, data in payloads:
        table += struct.pack("<HHIII", size, size, size * 4, cursor, len(data))
        blobs += data
        cursor += len(data)
        pad = (-cursor) % ALIGN
        blobs += b"\0" * pad
        cursor += pad

    header = struct.pack("<4sHHIHH16s", MAGIC, VERSION, len(payloads),
                         FLAG_PREMULTIPLIED, biggest, biggest, b"\0" * 16)
    body = bytearray(header + table)
    body += b"\0" * (offset - len(body))
    body += blobs

    out = os.path.join(OUTDIR, name + ".eic")
    with open(out, "wb") as f:
        f.write(body)
    if not quiet:
        tag = "  (bootstrap from .pam)" if bootstrap else ""
        print(f"  {name+'.eic':<24} {len(levels)} levels  "
              f"{len(body)//1024:>4} KB   <- {src}{tag}")
    return out


def discover():
    """Icon names to build: every PNG master, plus legacy .pam bootstraps."""
    names = set()
    if os.path.isdir(MASTERS):
        for f in os.listdir(MASTERS):
            if f.lower().endswith(".png"):
                names.add(os.path.splitext(f)[0])
    if os.path.isdir(OUTDIR):
        for f in os.listdir(OUTDIR):
            if f.endswith(".pam"):
                names.add(os.path.splitext(f)[0])
    return sorted(names)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ladder", default=",".join(str(s) for s in DEFAULT_LADDER),
                    help="comma-separated pixel sizes to generate")
    ap.add_argument("--list", action="store_true",
                    help="show what would be built, and from which master")
    ap.add_argument("names", nargs="*", help="icon names (default: all)")
    args = ap.parse_args()

    ladder = sorted({int(s) for s in args.ladder.split(",") if s.strip()})
    names = args.names or discover()
    if not names:
        print("mkicons: nothing to build "
              f"(no PNGs in {os.path.relpath(MASTERS, REPO)}/)")
        return 0

    if args.list:
        Image = _need_pillow()
        print(f"ladder: {ladder}")
        for n in names:
            try:
                _, src, bootstrap = load_master(n, Image)
                tag = " (bootstrap -- add a PNG master to improve)" if bootstrap else ""
                print(f"  {n:<16} <- {src}{tag}")
            except FileNotFoundError as e:
                print(f"  {n:<16} !! {e}")
        return 0

    # The generated .eic are committed, so a checkout builds without Pillow.
    # Only insist on it when there is actually something to (re)generate --
    # otherwise a missing optional dependency would break the whole image build
    # for someone who never touched the art.
    try:
        import PIL  # noqa: F401
    except ImportError:
        missing = [n for n in names
                   if not os.path.exists(os.path.join(OUTDIR, n + ".eic"))]
        if not missing:
            print("mkicons: Pillow not installed; keeping the committed .eic "
                  "(install Pillow to rebuild icons from masters)")
            return 0
        sys.exit("mkicons: Pillow is required to build "
                 f"{', '.join(missing)} (pip install Pillow)")

    Image = _need_pillow()
    os.makedirs(OUTDIR, exist_ok=True)
    print(f"mkicons: {len(names)} icon(s), ladder {ladder}")
    for n in names:
        try:
            build_icon(n, ladder, Image)
        except FileNotFoundError as e:
            print(f"mkicons: {e}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
