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
| Window controls as **traffic lights** — #FF5F57 and #28C840, 13px dots, glyph on hover only | **Three lights, ours**: the round shape kept, but close is the *danger* tone, zoom is the *accent*, minimize a quiet slate — and every glyph is drawn at rest | The round light is right and worth keeping: colour is legible at any size and from the corner of the eye, where a row of grey glyph-boxes is not. What is not kept is red/amber/green, and hiding the symbol until you are already touching it — the Mac can lean on a generation knowing which colour closes, and nobody has learned ours |
| The zoom control as a **toggle** (bigger / not bigger) | A **door**: resting on it opens a board of six placements — Left, Right, Top, Bottom, Fill, Full, each drawn as a little screen with the window's share filled in | "Make it bigger" is a guess about what you meant. Offering the placements makes it a choice, and it is the one place where a window manager can add real capability without adding a mode. Clicking the light itself still just fills, for when you did not want to choose |
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
4. **Nothing borrowed from Windows either.** Square control boxes in a row are
   as much someone else's face as three coloured dots, and an OS that avoids
   one by adopting the other has changed who it is copying, not stopped. The
   controls here are round because the round light is genuinely the better
   shape -- colour reads at any size and from the corner of the eye -- and
   what makes them OURS is the palette, the always-drawn glyphs, and the board
   behind the zoom light, not the outline.
5. **Geometry comes from the theme**, not from a constant typed at the call
   site: `radius_lg`, the 8px spacing grid, the type scale. Restyling the OS
   stays one change.
6. **Units are stated.** CPU is per core, like every `top` ever written, so
   four busy cores read 400% — a real number with a real unit, needing no
   knowledge of the core count (which userspace cannot ask for anyway, and
   which is not worth inventing a lie about).

## Fill and Full are two different things, on purpose

The one distinction the zoom board exists to make. **Fill** takes the work
area — everything that is not the top bar and not the dock — so the shell stays
reachable. **Full** takes the display, bar and dock covered, for a video or a
game. Every system has both and most make you discover which of two
similar-looking buttons is which; this one draws them side by side, names them,
and shows the difference in the picture (Fill's diagram has the two strips the
window does not cover).

The distinction needed a kernel change to be real. `compositor_win_move` clamps
every ordinary app window into the work area — which is what stops a window
being dragged under the bar or the dock, and is worth keeping — so asking for
the whole display and y=0 quietly put the window back under the bar, and Full
did exactly what Fill did. A window whose content is the size of the display is
now exempt from that clamp. The exemption is DERIVED from the window's size
rather than declared by the app, so an app cannot leave the flag set after it
shrinks again.

## The dock says what the machine is running, not what it started

Its sockets used to be lit purely from the desktop's own record of spawns, so
an app started from the shell showed no socket even with its window on screen.
It reads the compositor's window list now (`embk_win_list`), and keeps the
spawn handles for the half-second between a click and the app's first window —
where there is nothing on screen yet, and a dark socket reads as a click that
did not work.

The match is on the **binary's basename**, not the window title. A title is
what an app calls a window and it changes as the person works: open a document
and the editor's window becomes the document's name, so a dock that recognised
apps by title would light a tile only until the app was used. The kernel
records the basename at spawn (`struct process::exec_name`) and the syscall
layer annotates the window list with it. The name carries no authority and
nothing decides with it — a process's rights are still its namespace and its
capability set — and it is a basename rather than a path because the path would
hand anything that can see a window list a map of the filesystem for free.

Clicking a lit tile the dock did not launch raises that app rather than
spawning a second copy of something you can already see.

## Still open

* **Icons.** The dock and desktop art is abstract placeholder glyphs, which is
  why the dock has to name what you point at. Blocked on the icon pipeline.
* **Where an application's menus live** is undecided. Saying nothing is the
  current answer, and a better one than miming a menu bar that has no app
  behind it.
