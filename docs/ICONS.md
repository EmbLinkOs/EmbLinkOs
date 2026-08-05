# EmbLink icons — real art in, every size out

The desktop asks for an icon at whatever pixel size the moment calls for: 64 on
the desktop, 56 in the Apps grid, 44 for a drag ghost, 40 in the dock. Before
this system there was one 96×96 master per icon and the renderer sampled it
nearest-neighbour, so every one of those sizes threw away pixels on an
irregular grid — the jaggy, uneven look. Worse, five of eight apps shipped the
same `icon-launcher.pam`, so most of the desktop was one hummingbird.

The fix is a pipeline, not a bigger PNG:

    icons/masters/<name>.svg        <- real art, vector
        |  tools/mkicons.py  (build time; each level rasterised natively)
        v
    system/images/<name>.eic        <- ONE file holding every size
        |  em_image_at(path, want_px)  (runtime, picks a level, zero-copy)
        v
    a 1:1 blit at the size actually asked for

Because the container already holds the exact sizes the UI uses, the common
case does no resampling at all — the renderer copies pixels straight across.

## Dropping in art

Put an SVG in `icons/masters/` named after the icon:

    icons/masters/files.svg      ->  /system/images/files.eic
    icons/masters/terminal.svg   ->  /system/images/terminal.eic

Then `make embkfs.img`. A non-square SVG is letterboxed into the square
viewport rather than stretched, so design on a square canvas —
`viewBox="0 0 64 64"` is a good default.

### Why SVG rather than a big PNG

Every level is rasterised **natively at its own size**: the 16px level is drawn
as a 16px icon, not squeezed down from a 512px bitmap. No level is a downscale
of another, so small sizes keep their contrast instead of turning to mush, and
there is no largest-level ceiling to bump into.

It also renders through cairo's `ARGB32`, whose memory layout on little-endian
is *exactly* the premultiplied B,G,R,A that `.eic` stores — so the render lands
in the container with no conversion, no premultiply pass, and no filter
overshoot to clamp.

PNG masters still work (`icons/masters/<name>.png`) for art that is genuinely
raster — photographs, painted texture. Those are premultiplied and reduced with
LANCZOS, and the generator warns when the master is smaller than the top of the
ladder, since upscaling cannot invent detail.

### What the generator needs

- **SVG masters**: librsvg + cairo (`sudo apt install librsvg2-2 libcairo2` on
  Debian/Ubuntu). They are driven directly through `ctypes` rather than
  gobject-introspection, because marshalling a cairo context through GI needs
  the separate `python3-gi-cairo` package — one less thing to install.
- **PNG and legacy `.pam` masters**: Pillow.

Neither is needed for an ordinary build: the generated `.eic` are committed, so
a checkout without the rasterisers keeps what is in the tree and says so.

An app points at its icon through its `.app` manifest ([USERSPACE_v2.md](USERSPACE_v2.md)):

    name Files
    icon /system/images/files.eic

Nothing else changes — the desktop reads the manifest, and the size is chosen
by whichever widget draws it.

## Themed icons (tinting)

An icon can be drawn as a **stencil**: its alpha becomes coverage and the colour
comes entirely from the caller, so real art behaves exactly like a font glyph
and follows the palette instead of staying whatever colour it was authored in.

    ImageButtonTinted("/system/images/launcher.eic", 32, t->accent)

That is how the top bar's launcher button works. The art is authored in its own
colour, but in the bar it renders in the theme accent alongside the glyph-based
controls, and a theme change recolours it.

Use it for single-colour, glyph-like marks. Full-colour art (a folder, an app
logo) should be drawn untinted with `ImageButton`, since flattening it to one
colour throws away exactly what makes it recognisable.

The tint applies at raster time (`draw_image`'s `tint` argument), so it costs no
extra memory and no second copy of the icon.

## Sizes

The default ladder is **16, 24, 32, 40, 48, 56, 64, 96, 128**, chosen so that
every size the shell currently requests lands on an exact level. Change it with
`tools/mkicons.py --ladder 16,32,64,128,256`; the runtime adapts, since it reads
the level table out of the file rather than assuming a fixed set.

Selection rule at runtime: **the smallest level that is at least the requested
size**. Never upscale a smaller level (that blurs); shrinking slightly from the
next level up stays sharp. If the request exceeds the largest level, the largest
is used.

## The `.eic` container

All integers little-endian. One file, header then level table then pixels.

    Header (32 bytes)
      0   4   magic     "EICO"
      4   2   version   1
      6   2   n_levels
      8   4   flags     bit0 = RGB premultiplied by alpha (always set in v1)
      12  2   base_w    largest level's width
      14  2   base_h
      16  16  reserved (zero)

    Level table (n_levels × 16 bytes, ascending by size)
      0   2   width
      2   2   height
      4   4   stride    bytes per row (= width × 4)
      8   4   offset    from file start, 16-byte aligned
      12  4   bytes     (= stride × height)

    Pixel data, rows top to bottom, one little-endian uint32 per pixel:
      (a << 24) | (r << 16) | (g << 8) | b      with r,g,b PREMULTIPLIED

That last line is the whole point of the format: it is byte-for-byte the layout
the compositor and the software renderer already use
(`EMBK_PIXFMT_BGRA8888_PRE` — in memory the bytes read B, G, R, A). So loading
an icon is a header parse and a pointer into the file buffer. There is no decode
step, no per-pixel conversion, and no second allocation for the pixels.

### Why premultiplied, and why resize in that space

Resizing straight (non-premultiplied) RGBA blends the colour of fully
transparent pixels into their visible neighbours — the classic dark or white
halo around a soft edge. The generator premultiplies *first*, then resizes, so
transparent pixels contribute nothing but their (zero) weight.

LANCZOS is a sharpening filter and overshoots at hard edges, which can push a
channel above its alpha — impossible for premultiplied pixels and visible as a
bright fringe. The generator clamps every channel to its alpha afterwards.

## Regenerating

`make embkfs.img` regenerates any `.eic` whose master is newer. To run it alone:

    python3 tools/mkicons.py            # all masters
    python3 tools/mkicons.py --list     # what would be built, and from what

Until an icon has a real master, the generator falls back to the legacy
`system/images/<name>.pam` so the desktop keeps working; those bootstrap icons
are marked in `--list` output and are limited by their 96×96 source.
