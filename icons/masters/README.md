# Icon masters — drop real art here

One **SVG** per icon, named after the icon. `make` turns each one into a
`system/images/<name>.eic` carrying every size the shell asks for:

    icons/masters/files.svg     ->  /system/images/files.eic
    icons/masters/terminal.svg  ->  /system/images/terminal.eic

Then point an app at it from its `user/bin/<name>.app` manifest:

    name Files
    icon /system/images/files.eic

## Why SVG

Each size in the container is rasterised **natively at that size** — the 16px
level is drawn as a 16px icon, not squeezed down from a big bitmap. Nothing is
a downscale of anything else, so small icons stay crisp and there is no
"master too small" ceiling.

PNG masters (`<name>.png`) still work for genuinely raster art, but they are
reduced by filtering, so author those at 512² or larger.

## What makes a good master

- **Square canvas.** `viewBox="0 0 64 64"` is a good default. A non-square SVG
  is letterboxed into the square viewport, never stretched, so it will just sit
  smaller than its neighbours.
- **Legible at 16px.** The same art is used for the smallest level, where fine
  detail simply disappears. Strong silhouette first, detail second.
- **Plain SVG.** librsvg covers paths, shapes, strokes, gradients, clips and
  opacity. It does *not* run scripts, and external references (linked fonts or
  images) will not resolve — convert text to paths before saving.

## Template

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="4" y="10" width="56" height="44" rx="8" fill="#2B3140"/>
  <path d="M16 26 L26 33 L16 40" fill="none" stroke="#7CF08A" stroke-width="4"
        stroke-linecap="round" stroke-linejoin="round"/>
  <line x1="32" y1="41" x2="46" y2="41" stroke="#9AA3B8" stroke-width="4"
        stroke-linecap="round"/>
</svg>
```

## Organising into folders

Subfolders are fine and purely organisational — an icon is known by its
**filename** wherever it sits, so moving art between folders never changes the
path apps reference:

    icons/masters/svg/list.svg   ->  /system/images/list.eic

The one rule that follows from that: two masters cannot share a filename. If
they do, the generator says which two rather than silently picking one.

Run `python3 tools/mkicons.py --list` to see every master it found and where
each one came from.

Full format and pipeline notes: [docs/ICONS.md](../../docs/ICONS.md).
