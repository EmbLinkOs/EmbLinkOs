#!/usr/bin/env python3
"""Build .eic multi-resolution icon containers from real art.

    icons/masters/<name>.svg  ->  system/images/<name>.eic     (preferred)
    icons/masters/<name>.png  ->  system/images/<name>.eic

One master in, every size the shell asks for out. See docs/ICONS.md for the
container layout and the reasoning.

SVG is the preferred master because each level is rasterised NATIVELY at its
own size -- a 16px icon is drawn as a 16px icon, not squeezed down from a big
bitmap -- so small sizes stay crisp. It also renders through cairo's ARGB32,
whose memory layout on little-endian is B,G,R,A premultiplied: byte for byte
the .eic payload, so there is no conversion step at all.

Until an icon has a master we fall back to the legacy system/images/<name>.pam
so the desktop keeps working (those are capped by their 96x96 source).
"""

import argparse
import ctypes
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTERS = os.path.join(REPO, "icons", "masters")
OUTDIR = os.path.join(REPO, "system", "images")

# Chosen so every size the shell currently requests (40 dock, 44 ghost -> 48,
# 56 grid, 64 desktop -- each drawn at size-8) lands on an exact level.
DEFAULT_LADDER = [16, 24, 32, 40, 48, 56, 64, 96, 128]

MAGIC = b"EICO"
VERSION = 1
FLAG_PREMULTIPLIED = 1
HEADER_SIZE = 32
LEVEL_ENTRY_SIZE = 16
ALIGN = 16

CAIRO_FORMAT_ARGB32 = 0


# --------------------------------------------------------------------------
# SVG rasterisation (librsvg + cairo, driven directly through ctypes)
#
# The GI bindings can't be used here: marshalling a cairo Context through
# gobject-introspection needs python3-gi-cairo, which is a separate package.
# The shared libraries themselves are what actually do the work, so we call
# them directly and depend on nothing beyond librsvg being installed.
# --------------------------------------------------------------------------

class _CairoRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]


class SvgRenderer:
    def __init__(self):
        try:
            self.cairo = ctypes.CDLL("libcairo.so.2")
            self.rsvg = ctypes.CDLL("librsvg-2.so.2")
            self.gobj = ctypes.CDLL("libgobject-2.0.so.0")
        except OSError as e:
            raise RuntimeError(
                "SVG masters need librsvg and cairo.\n"
                "  Debian/Ubuntu:  sudo apt install librsvg2-2 libcairo2\n"
                f"  (dlopen failed: {e})")

        c, r, g = self.cairo, self.rsvg, self.gobj
        c.cairo_image_surface_create.restype = ctypes.c_void_p
        c.cairo_image_surface_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        c.cairo_create.restype = ctypes.c_void_p
        c.cairo_create.argtypes = [ctypes.c_void_p]
        c.cairo_destroy.argtypes = [ctypes.c_void_p]
        c.cairo_surface_destroy.argtypes = [ctypes.c_void_p]
        c.cairo_surface_flush.argtypes = [ctypes.c_void_p]
        c.cairo_surface_status.restype = ctypes.c_int
        c.cairo_surface_status.argtypes = [ctypes.c_void_p]
        c.cairo_image_surface_get_data.restype = ctypes.POINTER(ctypes.c_ubyte)
        c.cairo_image_surface_get_data.argtypes = [ctypes.c_void_p]
        c.cairo_image_surface_get_stride.restype = ctypes.c_int
        c.cairo_image_surface_get_stride.argtypes = [ctypes.c_void_p]

        r.rsvg_handle_new_from_file.restype = ctypes.c_void_p
        r.rsvg_handle_new_from_file.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
        r.rsvg_handle_render_document.restype = ctypes.c_int
        r.rsvg_handle_render_document.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p,
            ctypes.POINTER(_CairoRect), ctypes.c_void_p]
        r.rsvg_handle_get_intrinsic_size_in_pixels.restype = ctypes.c_int
        r.rsvg_handle_get_intrinsic_size_in_pixels.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_double),
            ctypes.POINTER(ctypes.c_double)]
        g.g_object_unref.argtypes = [ctypes.c_void_p]

    def open(self, path):
        err = ctypes.c_void_p()
        h = self.rsvg.rsvg_handle_new_from_file(
            os.fsencode(path), ctypes.byref(err))
        if not h:
            raise RuntimeError(f"{path}: librsvg could not parse this SVG")
        return h

    def intrinsic(self, handle):
        w = ctypes.c_double(0)
        h = ctypes.c_double(0)
        ok = self.rsvg.rsvg_handle_get_intrinsic_size_in_pixels(
            handle, ctypes.byref(w), ctypes.byref(h))
        if ok and w.value > 0 and h.value > 0:
            return w.value, h.value
        return 0.0, 0.0    # no intrinsic size -> just fill the viewport

    def render(self, handle, size):
        """Rasterise into size x size, returning tightly packed BGRA premul."""
        surf = self.cairo.cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size)
        if self.cairo.cairo_surface_status(surf) != 0:
            raise RuntimeError(f"cairo could not allocate a {size}x{size} surface")
        cr = self.cairo.cairo_create(surf)

        # Fit the artwork into the square viewport preserving aspect, centred,
        # so a non-square SVG is letterboxed rather than stretched.
        iw, ih = self.intrinsic(handle)
        if iw > 0 and ih > 0:
            scale = min(size / iw, size / ih)
            vw, vh = iw * scale, ih * scale
            rect = _CairoRect((size - vw) / 2.0, (size - vh) / 2.0, vw, vh)
        else:
            rect = _CairoRect(0.0, 0.0, float(size), float(size))

        err = ctypes.c_void_p()
        ok = self.rsvg.rsvg_handle_render_document(
            handle, cr, ctypes.byref(rect), ctypes.byref(err))
        self.cairo.cairo_surface_flush(surf)
        if not ok:
            self.cairo.cairo_destroy(cr)
            self.cairo.cairo_surface_destroy(surf)
            raise RuntimeError(f"librsvg failed to render at {size}px")

        stride = self.cairo.cairo_image_surface_get_stride(surf)
        data = self.cairo.cairo_image_surface_get_data(surf)
        raw = bytes(bytearray(data[:stride * size]))
        self.cairo.cairo_destroy(cr)
        self.cairo.cairo_surface_destroy(surf)

        # cairo pads rows to a 4-byte-aligned stride; .eic rows are tight.
        # cairo ARGB32 is already premultiplied and, little-endian, already
        # B,G,R,A -- so apart from the stride this is a straight copy.
        want = size * 4
        if stride == want:
            return raw
        return b"".join(raw[y * stride: y * stride + want] for y in range(size))

    def close(self, handle):
        self.gobj.g_object_unref(handle)


# --------------------------------------------------------------------------
# Raster masters (PNG, and the legacy .pam bootstrap)
# --------------------------------------------------------------------------

def _need_pillow():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("mkicons: Pillow is required for PNG/.pam masters "
                 "(pip install Pillow). SVG masters do not need it.")
    return Image


def read_pam(path):
    """Minimal P7 RGB_ALPHA / RGB reader -> (w, h, RGBA bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P7"):
        raise ValueError(f"{path}: not a PAM file")
    end = data.find(b"ENDHDR\n")
    if end < 0:
        raise ValueError(f"{path}: no ENDHDR")
    fields = {}
    for line in data[:end].decode("ascii", "replace").splitlines():
        parts = line.split()
        if len(parts) == 2:
            fields[parts[0]] = parts[1]
    w, h, depth = int(fields["WIDTH"]), int(fields["HEIGHT"]), int(fields["DEPTH"])
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


def raster_master(path, kind, Image):
    """Load a raster master, squared and premultiplied, ready to downscale."""
    if kind == "bootstrap":
        w, h, rgba = read_pam(path)
        img = Image.frombytes("RGBA", (w, h), rgba)
    else:
        img = Image.open(path).convert("RGBA")

    w, h = img.size
    if w != h:                    # pad, never stretch
        side = max(w, h)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(img, ((side - w) // 2, (side - h) // 2))
        img = canvas

    # Resizing must happen in premultiplied space, otherwise the colour of
    # fully transparent pixels bleeds into visible neighbours (edge halos).
    from PIL import ImageChops
    r, g, b, a = img.split()
    pm = Image.merge("RGBA", (ImageChops.multiply(r, a),
                              ImageChops.multiply(g, a),
                              ImageChops.multiply(b, a), a))
    return pm, img.size[0]


def raster_level(pm, size, Image):
    lv = pm.resize((size, size), Image.LANCZOS)
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
            out[i] = b
            out[i + 1] = g
            out[i + 2] = r
            out[i + 3] = a
            i += 4
    return bytes(out)


# --------------------------------------------------------------------------

def scan_masters():
    """{name: (path, kind)} for every master under icons/masters, recursively.

    Subdirectories are ORGANISATIONAL only -- an icon is known by its filename
    wherever it sits, so moving art between folders never changes the path apps
    reference. That means two masters cannot share a stem; we say so loudly
    rather than silently letting one win.
    """
    found = {}
    if not os.path.isdir(MASTERS):
        return found
    for root, dirs, files in os.walk(MASTERS):
        dirs.sort()
        for f in sorted(files):
            stem, ext = os.path.splitext(f)
            ext = ext.lower()
            if ext not in (".svg", ".png"):
                continue
            kind = "svg" if ext == ".svg" else "raster"
            path = os.path.join(root, f)
            if stem in found:
                prev, prev_kind = found[stem]
                # SVG beats PNG for the same name; anything else is a real clash
                if prev_kind == "svg" and kind == "raster":
                    continue
                if not (prev_kind == "raster" and kind == "svg"):
                    sys.exit(f"mkicons: two masters are both named '{stem}':\n"
                             f"  {os.path.relpath(prev, REPO)}\n"
                             f"  {os.path.relpath(path, REPO)}\n"
                             "Icon names come from the filename, so rename one.")
            found[stem] = (path, kind)
    return found


def find_master(name):
    """(path, kind) for an icon. A master anywhere under icons/masters wins;
    otherwise fall back to legacy system/images/<name>.pam art."""
    m = scan_masters().get(name)
    if m:
        return m
    pam = os.path.join(OUTDIR, name + ".pam")
    if os.path.exists(pam):
        return pam, "bootstrap"
    raise FileNotFoundError(f"no master for icon '{name}' "
                            f"(add icons/masters/{name}.svg)")


def pack(name, payloads):
    table_size = LEVEL_ENTRY_SIZE * len(payloads)
    offset = (HEADER_SIZE + table_size + ALIGN - 1) & ~(ALIGN - 1)

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

    biggest = max(s for s, _ in payloads)
    body = bytearray(struct.pack("<4sHHIHH16s", MAGIC, VERSION, len(payloads),
                                 FLAG_PREMULTIPLIED, biggest, biggest, b"\0" * 16))
    body += table
    body += b"\0" * (offset - len(body))
    body += blobs

    out = os.path.join(OUTDIR, name + ".eic")
    with open(out, "wb") as f:
        f.write(body)
    return out, len(body)


def build_icon(name, ladder, svg=None, quiet=False):
    path, kind = find_master(name)
    levels = sorted({s for s in ladder if s > 0})

    if kind == "svg":
        if svg is None:
            svg = SvgRenderer()
        handle = svg.open(path)
        try:
            # Every level rasterised natively at its own size -- the reason SVG
            # masters beat a big PNG: no level is a downscale of another.
            payloads = [(s, svg.render(handle, s)) for s in levels]
        finally:
            svg.close(handle)
    else:
        Image = _need_pillow()
        pm, master_side = raster_master(path, kind, Image)
        if master_side < max(levels):
            print(f"mkicons: WARNING: {name}: master is {master_side}px but the "
                  f"ladder goes to {max(levels)}px -- larger levels are upscaled "
                  f"and will look soft. An SVG master has no such limit.",
                  file=sys.stderr)
        payloads = [(s, raster_level(pm, s, Image)) for s in levels]

    out, nbytes = pack(name, payloads)
    if not quiet:
        tag = {"svg": "", "raster": "", "bootstrap": "  (bootstrap from .pam)"}[kind]
        print(f"  {name+'.eic':<24} {len(levels)} levels  {nbytes//1024:>4} KB   "
              f"<- {os.path.relpath(path, REPO)}{tag}")
    return out


def discover():
    names = set(scan_masters())
    if os.path.isdir(OUTDIR):
        for f in os.listdir(OUTDIR):
            if f.endswith(".pam"):
                names.add(os.path.splitext(f)[0])
    return sorted(names)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
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
              f"(no masters in {os.path.relpath(MASTERS, REPO)}/)")
        return 0

    if args.list:
        print(f"ladder: {ladder}")
        for n in names:
            try:
                path, kind = find_master(n)
                tag = {"svg": "", "raster": "  (PNG -- an SVG master scales better)",
                       "bootstrap": "  (bootstrap -- add a master to improve)"}[kind]
                print(f"  {n:<16} <- {os.path.relpath(path, REPO)}{tag}")
            except FileNotFoundError as e:
                print(f"  {n:<16} !! {e}")
        return 0

    # The generated .eic are committed, so a checkout builds without the
    # rasterising toolchain. If it's absent but every output already exists,
    # keep what's committed rather than break the image build for someone who
    # never touched the art. (Staleness is make's job -- the Makefile rule
    # depends on the masters and on this script, so reaching here means
    # something really did change.)
    if not _can_rasterise(names):
        missing = [n for n in names
                   if not os.path.exists(os.path.join(OUTDIR, n + ".eic"))]
        if not missing:
            print("mkicons: no rasteriser available; keeping the committed "
                  ".eic (install librsvg for SVG, Pillow for PNG masters)")
            return 0
        sys.exit(f"mkicons: cannot build {', '.join(missing)} -- "
                 "install librsvg2-2 (SVG masters) or Pillow (PNG/.pam)")

    os.makedirs(OUTDIR, exist_ok=True)
    print(f"mkicons: {len(names)} icon(s), ladder {ladder}")
    svg = None
    for n in names:
        try:
            path, kind = find_master(n)
            if kind == "svg" and svg is None:
                svg = SvgRenderer()
            build_icon(n, ladder, svg=svg)
        except (FileNotFoundError, RuntimeError) as e:
            print(f"mkicons: {e}", file=sys.stderr)
            return 1
    return 0


def _can_rasterise(names):
    """Is the toolchain for these masters' kinds actually present?"""
    kinds = set()
    for n in names:
        try:
            kinds.add(find_master(n)[1])
        except FileNotFoundError:
            pass
    if "svg" in kinds:
        try:
            SvgRenderer()
        except RuntimeError:
            return False
    if kinds & {"raster", "bootstrap"}:
        try:
            from PIL import Image  # noqa: F401
        except ImportError:
            return False
    return True


if __name__ == "__main__":
    sys.exit(main())
