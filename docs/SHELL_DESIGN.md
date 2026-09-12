# The shell's design language

EmbLinkOS has a top bar and a dock. That much is a deliberate choice and it is
not going to change: they are a good shape for a desktop, and the Mac is the
best argument for them. **Basing the shape on macOS is allowed. Wearing its
face is not.** This document is the difference, written down, because the
difference is not self-enforcing — the code drifted back into a costume twice
before anyone noticed, and both times the comment above the code said exactly
which Apple design it was reproducing.

Checked against the source on 2026-09-12.

## The rule everything else follows

> **The Mac dock reacts to your cursor. Ours reacts to the machine.**

EmbLinkOS measures itself more thoroughly than it does almost anything else —
per-core idle residency, a scheduler policy you can name and swap, a page cache
that reports its own hit rate, per-process CPU in `embk_proc_list`. A shell for
*this* OS should show the machine. Delight that reports only the position of
your hand is delight another system already did better.

## What was removed, and what replaced it

| Was | Is | Why |
|---|---|---|
| Dock icons magnified by pointer distance — the Mac dock's defining gesture, implemented as that | Icons never move or resize | A target that changes size under the pointer is a worse target. The Mac pays that price knowingly for the delight; we are not taking the delight, so we do not owe the price |
| A 4px dot under a running app | A lit **socket**: the whole tile gets a filled plate and an accent hairline | Whether an app is running is the most useful thing the dock knows. A dot is a footnote; a socket is visible from across the room |
| Window controls as **traffic lights** — #FF5F57 and #28C840, 13px dots, glyph on hover only | One **cluster**: three segments (minimize, maximize, close) in a single hairline frame, glyphs always drawn, colour only on the segment under the pointer | Two saturated dots per window are the loudest thing on a desktop full of windows, and this OS spends boldness on one accent. A glyph you can only see on hover is unusable to anyone who has not already learned it — and nobody has learned ours |
| Menu bar carrying **File / Edit / View** | Nothing — only the real system menu | They were static. Twelve words (New, Open, Undo, Redo, Zoom In, Zoom Out) wired to nothing. A Mac menu bar carries the focused app's menus, so this one grew app-shaped menus with no app behind them |
| Four status glyphs: a star, a bolt, a gear, a heart | `CPU nn%` with a small meter, sampled from `embk_proc_list` | They were connected to nothing. The bar now reports the one thing this OS can state precisely |
| A hard-coded clock reading **9:41** — Apple's marketing time | The real clock | (Fixed earlier; kept here because it is the same mistake as the glyphs, and it is the clearest example of it) |

The dock is still a floating rack clear of the screen edge, still glass, still
centred. Its corner radius is now the theme's own `radius_lg`, the curve every
card and panel in the OS uses — a dock cut from the system's shape language
belongs to the system; a pill belongs to whoever invented that pill.

## Standing rules

1. **Every element reports something true.** If the OS cannot measure it, the
   shell does not show it. A placeholder that looks like a reading is worse
   than a gap, because a gap is honest about being a gap. There is no battery
   driver, so there is no battery icon — and the CPU readout is *labelled*,
   because an unlabelled percentage in a menu bar is read as a battery by
   everyone who has ever used a computer.
2. **Colour at rest is expensive.** One accent, spent on state that matters
   (a running app, a focused control). Chrome is hairlines and neutrals.
3. **A control says what it does.** Glyphs are drawn at rest, not revealed on
   hover. Convention can only be leaned on where the convention exists, and
   this OS has none yet.
4. **Geometry comes from the theme**, not from a constant typed at the call
   site: `radius_lg`, the 8px spacing grid, the type scale. Restyling the OS
   stays one change.
5. **Units are stated.** CPU is per core, like every `top` ever written, so
   four busy cores read 400% — a real number with a real unit, needing no
   knowledge of the core count (which userspace cannot ask for anyway, and
   which is not worth inventing a lie about).

## Still open

* **The dock only knows what it launched.** Its sockets are lit from the
  desktop's own record of spawns, so an app started from the shell shows no
  socket even while its window is on screen. Fixing it properly needs the
  compositor to expose a window list (pid, title, minimized) — a process here
  has no name to match on, by design: it is a capability system, and processes
  are named by handle. Recorded in docs/TODO.md.
* **Icons.** The dock and desktop art is abstract placeholder glyphs, which is
  why the dock has to name what you point at. Blocked on the icon pipeline.
* **Where an application's menus live** is undecided. Saying nothing is the
  current answer, and a better one than miming a menu bar that has no app
  behind it.
