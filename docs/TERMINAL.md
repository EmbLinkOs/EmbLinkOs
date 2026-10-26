# Terminal sessions and the development shell

EmbLinkOS now has private pseudo-terminal pairs. Terminal launches the native
shell on a terminal slave; programs inherit that slave through standard input,
output and error. The emulator owns the master. The emulator now supplies a real VT screen model.
An initial spawn-based `sh` port is available through `sh`, `sh file.sh` and
`sh -c 'command'`; see [POSIX_SHELL.md](POSIX_SHELL.md) for its language,
utilities, limits and verification. This does not claim that zsh or arbitrary
Unix applications run here.

## Implemented contract

- Native syscall `terminal` (148) creates a pair of **file descriptors** and
  controls the device through a descriptor. No public global terminal ID or
  ambient access to another session is introduced. Existing syscall IDs and
  the 14 capability IDs are unchanged.
- `embk_openpty(pair)` returns master/slave descriptors. Spawn action
  `EMBK_SPAWN_ACTION_INHERIT_FD` (9) explicitly copies a parent descriptor into
  a child slot; its `src_obj_handle` field carries the parent **fd** for this
  action. Existing object-handle actions retain their meaning.
- `read`, `write`, `dup`, inherited stdio, `fstat`, native readiness polling,
  `select`'s existing readiness adapter and nonblocking I/O dispatch through
  the new descriptor backing. PTYs report character-device type, so child
  `isatty` and newlib's terminal buffering detection see an actual terminal.
- Each pair has its own attributes, dimensions, canonical line, input/output
  queues and foreground interrupt target. The machine console keeps its old
  discipline and interrupt route.
- Canonical mode supports newline records, erase, line kill and Ctrl+D. EOF is
  a single record: Ctrl+D on an empty line produces one zero-byte read; input
  after that remains readable. Ctrl+D after text terminates that text record
  without adding a second empty EOF. Partial reads preserve record boundaries.
- Raw mode delivers bytes without waiting for Enter. `VMIN=0` or `1` with
  `VTIME=0` is supported. Canonical-to-raw changes preserve pending text;
  `TCSAFLUSH` intentionally discards pending input. Unsupported termios flags,
  timed reads and `TCSADRAIN` return errors instead of claiming support.
- Supported attributes are `ICRNL`, `OPOST`/`ONLCR`, `CS8`, `ICANON`, `ECHO`,
  `ECHOE`, `ECHOK` and `ISIG` for `VINTR`. `cfmakeraw` selects the supported raw
  subset. The legacy console still refuses `tcgetattr`/`tcsetattr`.
- `TIOCGWINSZ`/`TIOCSWINSZ` share measured character dimensions. A newly created
  device reports zero dimensions until its emulator supplies a measurement.
  Terminal updates its dimensions as its viewport changes.
- Ctrl+C uses the OS's existing counted interrupt/cooperative cancellation
  model. A shell names itself or a child it holds a spawn handle for. The
  native shell catches interrupts for its own work, delegates to foreground
  children, then restores its route and prompt attributes. Its existing
  grace-then-kill policy handles children that decline cancellation.
- Last-master close hangs up the slave and cancels its current foreground
  target; blocked readers/writers wake. Buffered output drains before slave
  hangup becomes EOF at the master. Duplicates retain the endpoint until its
  last descriptor closes. An in-flight I/O pin protects memory without keeping
  a closed endpoint alive. Hard-killed threads release abandoned I/O pins during
  reaping. Rejected spawns release inherited descriptors.

Pairs are allocated as they are opened and freed with their last reference;
up to 64 are live at once (the next open gets `ENFILE`). Each has 4096 queued
input cells and a 32 KiB output queue -- what a terminal takes in between two
frames. Canonical lines retain up to 1023 bytes plus the
newline; further line bytes are discarded until an editing/termination key.
Queue pressure returns partial writes or `EAGAIN`, or sleeps for blocking I/O.
Canonical editing operates on bytes; Unicode-aware erase is still pending.

## Terminal application

Both shells edit their own lines ([SHELL.md](SHELL.md#10-the-line--the-shell-edits-its-own)):
the native shell and `sh` draw their prompts, read every key in raw mode, and
recall, search and complete themselves. Terminal is a screen for them, as for
any program -- keys are encoded and sent, output is drawn -- and it has no line
editor of its own any more (until 2026-10 it edited the native prompt locally
and sent the finished line). A prompt mark with `edit=shell` says so;
a program that marks a prompt without it simply gets its keys like any other.

- Shift+PgUp/PgDn and the wheel page the scrollback; Shift+Home/End go to its
  top and back to the live screen.
- Escape, Ctrl+D, Ctrl+L and everything else go to the program. Once the shell
  has exited, Escape closes its tab (the window, for the last one).
- **Clear** empties the scrollback and the screen; at a shell prompt it asks
  the shell to redraw (Ctrl+L) instead of wiping the prompt from under it.
- **Paste** is bracketed when the program asked for it (both shells do at their
  prompts): the text arrives as text and nothing runs until Enter. Without it, a
  line break is sent as Enter, as Terminal.app pastes.

An interactive program can read a canonical line, disable echo, or select raw
mode.

### Tabs, selection, find, links

**Tabs.** Each tab is a session of its own: its PTY, its shell, its screen,
scrollback and selection. Super+T opens one in the directory the shown tab is
in; Ctrl+Tab and Ctrl+Shift+Tab go between them (Super+1-6 are the spaces',
so Super+7, 8, 9 pick the seventh, eighth and last). The strip of tabs shows
once there are two, each named for its directory -- or for what the program
called itself (OSC 0/2) -- with its close button and a + for a new one; the
window's title is the shown tab's full directory. A tab whose shell exits says
so and closes on Escape. Super+W closes the shown tab, the last one the
window; the window's close light and Super+Q close them all -- Super+Q now
reaches an app as its own window action (`EMBK_WIN_ACTION_QUIT`, read in a
close hook as `em_close_reason()`), so an app with tabs can tell "this tab"
from "everything". A closed tab's master is closed (a hangup for its shell)
and the shell reaped once it has gone; the PTY is freed with it.

**Selection** (`user/terminal/screentext.c`). With the mouse not asked for by
the program -- or with Shift held when it is -- the left button selects: a
drag what it covers, a double click a word (a path or URL whole; brackets and
quotes not), a triple click the line, wrapped rows and all; Alt-drag a
rectangle; Shift-click extends; a drag past the top or bottom moves the view.
A selection holds rows by id, so it stays on its text as output scrolls it
into the scrollback; a resize ends it. Copy (Super+C, Ctrl+Shift+C, the menu)
takes the selection -- a wrapped line joined, trailing blanks dropped -- or,
with nothing selected, the visible screen. Super+A selects everything.

**Find.** Super+F opens a field over the screen. Typing finds the last match
on the screen or in the scrollback (ASCII case ignored); Enter, Super+G and
the up arrow find the one before, Shift+Enter, Super+Shift+G and the down
arrow the one after, going round at the ends. Every match on the screen is
marked, the one found more strongly, and the view moves to it. Escape or Done
closes the field.

**Links.** With Super held, a link under the pointer is underlined and named
in the title bar; Super+click opens it. A link is an OSC 8 hyperlink or a URL
in the text (a scheme with `://`, `mailto:`, `www.`), read across wraps, with
the sentence's full stop and an unmatched bracket left out. http and https
open in Vellum, a `file://` folder in Files and a file in Editor -- each given
what it reads at start (`VELLUM_URL`, `FILES_PATH`, `EDIT_FILE`).

**The menu.** The right button opens Copy, Paste, Select All, Find, Clear
Scrollback, New Tab, Close Tab -- and Open Link and Copy Link over a link.

**Zoom.** Super+= and Super+- make the font a pixel larger or smaller (8 to 32
px at the standard interface size), Super+0 puts it back; every tab reflows.

**Resizing a prompt.** A shell that edits its own line turns on DECSET 2048
while it does. When the window is resized during such a prompt, Terminal first
erases from the prompt's first row (recorded at the A mark, as rows scrolled
plus row, so it stays right while the screen scrolls) to the end of the
screen, puts the cursor there, resizes, and after the kernel has the new size
sends `CSI 48;rows;cols;height;width t`. The shell redraws its prompt and line
from that row at the new width. Without the erase, the old wrapped prompt
would be reflowed into a duplicate above the redrawn one. Turning 2048 on
also reports the current size at once, and a resize from before reports were
on is not reported later -- nothing was erased for it.

**Returning to the prompt (OSC 133;R).** EmbLink's own addition to the marks:
`ESC ] 133;R BEL` puts the cursor at the start of the row where the current
prompt began (kept as rows scrolled plus row, and re-marked after a prompt
resize). The shell's editor starts every redraw with it when `TERM_PROGRAM` is
`EmbLinkTerminal`, instead of counting rows back up, so a redraw that was
already on its way when the window changed still lands on the prompt. Other
terminals ignore an OSC they do not know, and the editor falls back to
relative motion there.

### Keys

The keyboard queue says which key produced every byte (`embk_key_poll_key`,
syscall 157): the byte, the modifiers when the key went down, the key itself
(an `EMBK_KC_*`, or its unshifted character) and flags. Up and Ctrl+S are both
0x13 in the character stream, and Backspace and Ctrl+H both 0x08; the key tells
them apart. F1-F12, Insert and Menu have no byte at all and arrive as entries of
their own, flagged `EMBK_KEYF_ONLY`, in order with the text; keypad keys carry
`EMBK_KEYF_KEYPAD`. Every reader that wants characters (`key_poll`,
`key_poll_input`, the console) skips those entries and sees exactly the bytes it
always saw. The toolkit hands the identity to a key hook through `em_key_code()`
and `em_key_flags()`, and delivers keys with no character only to an app that
asked (`em_key_hook_wants_key_only`), as `EM_KEY_ONLY|code`. PS/2, USB (boot
protocol, directly or behind a hub), I2C HID and virtio keyboards all stamp the
key; a USB keyboard now goes through the same decoding as the others, so it has
Ctrl, Alt, GUI shortcuts, layouts and F-keys, and its keys are released when it
is unplugged.

Terminal encodes keys as xterm does with its PC-style keys:

| Key | Sent |
| --- | --- |
| arrows, Home, End | `CSI A`..`D`, `H`, `F` (`SS3` in application cursor mode); `CSI 1;m x` with modifiers |
| Insert, Delete, PgUp, PgDn | `CSI 2~` `3~` `5~` `6~`; `CSI n;m~` |
| F1-F4 | `SS3 P`..`S`; `CSI 1;m P`..`S` |
| F5-F12 | `CSI 15~ 17~ 18~ 19~ 20~ 21~ 23~ 24~`; `CSI n;m~` |
| Backspace / Ctrl+Backspace | DEL / `^H` (a profile can swap them) |
| Alt+key | ESC before the key's bytes |
| Ctrl+Space, 2..8, `[ \ ] ^ _ /` | NUL, ESC, FS, GS, RS, US, DEL as xterm sends them |
| keypad | its character; `SS3 p`..`y n M k m j o` in application keypad mode (`ESC =`) |

`m` is 1 + Shift(1) + Alt(2) + Ctrl(4). GUI shortcuts never reach a program.
Keys are written to the PTY as they are pressed, not on the next idle pass.

The emulator master is nonblocking, with a bounded pending-input queue so a
slow program cannot block the UI thread. Output draining has a per-iteration
budget so continuous output also yields to rendering. Overflow is reported;
input is not silently discarded. Closing the emulator closes the master before collecting
its shell. Appearance and display scaling follow the Contour desktop.

## Screen model and interactions

The screen model (`user/terminal/`) is UI-free and host-tested: `vt_parse.c`
turns bytes into operations, `vt_grid.c` holds the cells, the two screens and
the scrollback, `vt_keys.c` encodes keys, mouse and focus events. The renderer
paints into retained bitmaps and redraws only when the model, input, size or
appearance changed. Output work is bounded per event-loop iteration.

**Cells.** A cell is 8 bytes: a codepoint (or the id of a cluster -- a base
character and up to seven combining marks), a style id, a width (1, 2, or 0 for
a wide character's right half) and flags. Styles are interned per terminal:
foreground, background and underline colour each kept as the program named it
(the default, a palette index, or RGB), the attributes, the underline style and
an OSC 8 link. Because a palette index stays an index, a palette change -- a
theme switch, OSC 4 -- recolours text already on the screen. When the style
table (65,535 looks) fills, unused ones are dropped and the rest renumbered;
if that frees too little, the oldest half of the scrollback goes, so a
scrollback full of truecolour images costs one pass per thousands of new
looks, not one per character. Erased cells keep only the pen's background
(xterm's BCE).

**The screen.** Any size up to 1000 columns by 500 rows. The primary screen's
rows that scroll off its top join the **scrollback as logical lines**: a line
that wrapped is stored once, unwrapped, and read back laid out at the current
width. The scrollback is capped in bytes (8 MB by default), not lines. A resize
**reflows** the primary screen every time -- after `clear`, after cursor
addressing, whatever ran -- and re-wraps the scrollback; the cursor keeps its
character, the shell's prompt mark keeps its row, rows that no longer fit above
the cursor go to the scrollback, and a full screen that grows taller brings
scrollback lines back. The alternate screen has no scrollback and keeps its
coordinates (clipped and padded); its program redraws on the new size.

**The parser** follows DEC's state machine (vt100.net): C0 controls act
anywhere, `;` separates parameters and `:` the sub-parameters of one, private
markers and intermediates are collected, `ESC` inside a sequence starts over,
CAN/SUB cancel, and OSC/DCS/SOS/PM/APC strings end at ST or BEL. Text is UTF-8;
a malformed sequence is one U+FFFD.

| Area | Sequences |
| --- | --- |
| Cursor and editing | CUU/CUD/CUF/CUB (stopping at the margins), CNL/CPL, CHA/HPA/HPR, CUP/HVP, VPA/VPR, CHT/CBT, HT, IND/NEL/RI, DECSC/DECRC, SCOSC/SCORC, ED (0-3), EL, ECH, ICH, DCH, IL/DL, SU/SD, DECSTBM, TBC/HTS, REP, DECALN (`ESC # 8`) |
| SGR | bold, dim, italic, underline with styles `4:0`-`4:5` and `21`, blink (5, 6), inverse, conceal, strike, overline (53/55), 16/256/RGB colours for text, background and underline (`38`/`48`/`58`, both `;` and `:` forms), and their resets |
| Modes | IRM, LNM; DECCKM, DECSCNM, DECOM, DECAWM, cursor blink (12), DECTCEM, 47/1047/1048/1049, DECNKM (66), DECBKM (67), mouse 9/1000/1002/1003 with encodings 1005/1006/1015/1016, focus 1004, alternate scroll 1007, Alt sends ESC 1036/1039, bracketed paste 2004, synchronized output 2026, in-band resize 2048; XTSAVE/XTRESTORE; DECSTR soft reset; RIS |
| Replies | DSR 5/6, DECXCPR, DA1 (a VT220 with ANSI colour), DA2, DA3, XTVERSION (`EmbLinkTerminal(1.0)`), DECRQM for ANSI and DEC modes, DECRQSS (`m`, `r`, ` q`, `"p`, `"q`), XTGETTCAP (TN, colours, RGB, styled underlines, synchronized output...), window reports `CSI 11/13/14/16/18/19 t`, the title stack (`CSI 22/23 t`) |
| OSC | 0/1/2 titles, 4 palette (set and query), 7 working directory, 8 hyperlinks, 10/11/12 colours (set and query, answered with the query's terminator), 52 clipboard write (off unless allowed; never read), 104 and 110-112 resets, 133 shell marks |
| Charsets | G0-G3 designation, SO/SI, LS2/LS3, DEC special graphics |

Programs that try to move, resize or retitle-report the window (`CSI 3/4/8/21
t`) are not answered: that is the person's window, not the program's.

`TERM=xterm-256color` and `COLORTERM=truecolor` name what this is;
`TERM_PROGRAM=EmbLinkTerminal` and `TERM_PROGRAM_VERSION` say which one. The
native shell turns colour on for any `TERM` but an unset one or `dumb`.

**Mouse and focus.** A program that asks for mouse reports (DECSET
9/1000/1002/1003) gets presses, releases, drags, motion and the wheel in the
encoding it chose, for all three buttons; Shift keeps the mouse for Terminal.
With reporting off, the wheel pages the scrollback -- or, in the alternate
screen (less, man, vim), sends Up and Down, as Terminal.app does. Focus
reports (DECSET 1004) say when the window gains and loses the keyboard
(`embk_win_focused`, syscall 158). DECSET 2026 holds the picture while a
program draws a frame in parts, for at most 200 ms. BEL flashes a frame
around the screen.

**Copy screen** exports the visible grid as UTF-8 text, without terminal escape
sequences. **Paste** removes unsafe control bytes; an application that enables
bracketed paste (both shells, at their prompts) receives its delimiters and
multiline content. Cmd+C/V and Ctrl+Shift+C/V operate on the screen clipboard;
ordinary Ctrl-letter input continues to the running program. Shift+PageUp/Down
and the wheel view primary history; the alternate screen has no history view.
**Clear** and the shell's `clear` builtin clear the screen and history (at a
shell prompt, Clear asks the shell to redraw it).

Shell integration uses the OSC 133 marks: `A` (prompt start; `edit=shell`,
`cwd=` last), `B` (input start), `C` (command start) and `D;status`
(command end), plus EmbLink's `R` (back to the prompt's start). The pending program-input queue holds 65,568 bytes, independently of the smaller
kernel device queues, and reports pressure in the app bar.

Shared text controls now receive a single paste command after the app hook gets
first refusal. Editors read multiline clipboard text directly; fields flatten
line breaks/tabs. This avoids overflowing the 32-key queue. Paste replaces a
selection as one undoable edit; text that cannot fit leaves the original intact.
Retained text runs explicitly clear their highlights after selection collapse.
Note++'s custom hook also bounds the full clipboard length before indexing its
buffer and accepts multiline text up to the 64 KiB clipboard limit.

### Fonts and the renderer

**The faces.** Terminal draws in MesloLGS Nerd Font Mono -- the face the
Mac's Powerlevel10k setup uses -- in four faces (regular, bold, italic, bold
italic), vendored with its licences in `third_party/fonts/meslo-nf` and
installed as `/system/fonts/meslo-nf-mono*.ttf` on both architectures
(`tools/embkfs_mkfs/fonts.py`, the one list both image builders use). The
size is 14 px at the standard interface size and follows the interface's
scale; at 14 px a cell is 8 x 18 pixels, the font's own advance and line
height. A character the face lacks is taken from regular Meslo, then DejaVu
Sans Mono, then DejaVu Sans (centred in its cells); one no face has is drawn
as a box the width of its cells.

**Drawn, not from a font** (`user/terminal/glyphs.c`): box drawing
U+2500-257F (light, heavy, double, dashed, rounded, diagonal, and every mix
of them), block elements U+2580-259F, braille U+2800-28FF and the Powerline
separators U+E0B0-E0BF. They fill the cell exactly at any size, so frames
and segments meet the next cell edge to edge -- no seams between rows, no
sliver of background at a separator's point.

**What a cell shows** (`user/terminal/render.c`): colours through the
model's palette; bold from the bold face (bold-as-bright, brightening ANSI
0-7, is a renderer option for Preferences); italic from the italic face; dim
as half-way to the ground; inverse, and DECSCNM over the whole screen;
underline in its five styles and its own colour; strike; overline; combining
marks over their base; blinking text every 500 ms. The cursor is a block
(the character under it in the ground colour), an underline or a bar
(DECSCUSR); styles 1, 3 and 5 -- and 0 with DECSET 12 -- blink every 530 ms,
restarting at each key and each output; without the keyboard it is an
outline.

**Repaints.** Each row has a signature of what it shows -- cells, resolved
colours, the cursor on it, the blink phase if it blinks -- and only rows
whose signature changed are repainted, into one retained buffer. When the
screen scrolled, last frame's rows are moved up and only the new rows drawn.
The repainted rows are the frame's damage: `ui_image_sized_damage` tells the
scene which part of the image changed, the scene renderer repaints and the
window presents only that (`scene_set_image_damage`). A key typed or a cursor
blink presents one row, not the window.

Measured under QEMU (x86-64 emulated on an Apple-silicon host, so every
instruction is translated) with `ttyprobe --bench`: output that replaces the
whole 156 x 34 screen every frame costs the renderer about 35 ms a frame, out
of about 640 ms a frame in all -- the rest is presenting and compositing the
whole window, which a screen that changed everywhere needs. The PTY's 4 KiB
output queue brings at most 4 KiB a frame, so that host sustains about 7 KiB/s
of scrolling output; M6's larger queue raises what a frame takes in.

**The font engine** (`ui/backend/font.c`, shared by every app) reads cmap
format 12 as well as 4, so icons in the supplementary private use planes
(Nerd Fonts' Material Design set) and other astral characters are found;
holds 16 fonts; reports a glyph the font lacks (`font_has_glyph`, and
`missing` on a cache entry) so a caller can fall back; and caches glyphs in a
hashed table over up to four atlas pages allocated as they are needed. When
they are full, the page used longest ago is emptied, not the whole cache.

### Compatibility limits

There is no CJK or emoji font on the image: those characters draw as boxes
two cells wide (the model and widths are right; M5 adds faces, not fonts for
every script). Colour emoji, emoji ZWJ sequences and ligatures need shaping
the font engine does not do. Combining marks are drawn at their base's
origin, where both shipped mono faces place them; a mark from a proportional
fallback may sit off-centre. OSC 52 writes and bold-as-bright need
Preferences to turn them on (M7). Split panes come after the prompt work.

Not offered: left/right margins (DECSLRM), double-width and double-height
lines, sixel and other inline images, the kitty keyboard protocol and
`modifyOtherKeys` (keys follow xterm's PC-style encoding), and bidirectional
text. Grapheme clusters are a base character and its combining marks; emoji
ZWJ sequences are separate cells. Resize dimensions are queryable and
reported in-band (DECSET 2048); there is no `SIGWINCH` yet.

## Installed development headers

The image ships `embk_terminal.h` in the SDK and installs the target's working
`termios.h`/`sys/ioctl.h` instead of newlib's missing-target forwarding header.
Shared `embk_*.h` declarations keep their relative `data/src/user/lib` location
beside staged kernel sources. A packaging test checks byte equality and unique
SDK paths even when upstream headers with the same names are present.

## Verification

```
make terminal-test terminal-screen-test terminal-golden-test terminal-render-test terminal-screentext-test font-test backend-test
make shell-test declare-test em-test kit-test     # shell-test includes the line editor on the screen model
# Boot the image, then use the serial debug console:
test terminal
test tty
test kbd ring
# Graphical acceptance with QEMU, GDB and the existing cloud build environment:
python3 tools/terminal_smoke.py
python3 tools/terminal_screen_smoke.py
python3 tools/terminal_xterm_smoke.py
python3 tools/terminal_render_smoke.py
python3 tools/terminal_tabs_smoke.py
```

`terminal-test` compiles the production device state machine with a minimal
scheduler shim. It tests randomized byte streams in both directions, canonical
bounds, EOF records, interrupt dispatch, allocation limits and pinned hangup
lifetimes. Blocking/wakeup and descriptor ownership are tested in the real guest,
not inferred from that shim. It also runs under AddressSanitizer/UBSan:

```
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ikernel tests/tty/pty_host.c -o /tmp/pty-host
ASAN_OPTIONS=detect_leaks=0 /tmp/pty-host
```

The guest `ttyprobe --selftest` covers canonical/raw input, EOF records,
per-device dimensions, duplicate and inherited descriptors, hard-killed blocked
readers/writers, nonblocking queue
pressure (the full 32 KiB of output), input flushing, independent sessions,
blocked-read hangup, faulty copyout rollback and rejected-spawn cleanup. It
also fills the system's 64 pairs: one process's 64 descriptors reach only about
30, so two `--hold` children take theirs first. The next open gets `ENFILE`,
and freed pairs open again. `test terminal` checks that device allocation
returns to its baseline afterward.

The graphical acceptance script drives actual QMP keyboard input. `ttyprobe`
without options demonstrates canonical editing, receives one raw key without
Enter, restores cooked input and finishes on Ctrl+D. It verifies prompt
history/editing, foreground Ctrl+C, shell-loop interruption and independent
Terminal windows, and records real screenshots.

The screen-model host suite (`make terminal-screen-test`) drives every
sequence above and checks it against what xterm does: cursor and editing,
margins and the scrollback's byte cap, both screens, every SGR form, palette
changes and queries, clusters and wide cells, reflow (wide to narrow and back,
after `clear`, the cursor, the prompt mark, the scrollback, pulling lines back,
the alternate screen clipped), every reply, OSC 4/7/8/10-12/52/104/133, mouse
encodings, keys, more than 65,535 styles at once, and split input giving the
same state as whole input. The same suite runs again under
AddressSanitizer/UBSan with 20,000 rounds of structured random input, random
resizes up to 1000 x 500, resets and history-limit changes, checking the
model's invariants (row ownership, wide-cell pairing, style and cluster ids,
scrollback accounting) as it goes.

`make terminal-render-test` renders real screens from the screen model with
the vendored Meslo faces (and the host's DejaVu as the fallback) and checks
the pixels: the cell metrics, box frames of every weight unbroken across
cells, a double frame's inner and outer corners, a Powerline separator's edge
and point, blocks, braille, shades, bold and italic from their faces,
bold-as-bright, the fallback chain and the box for what no face has,
combining accents, the three cursor shapes and the outline, row-level damage
(one changed row repaints one row; a blink phase only the blinking rows; a
palette change the rows it colours; a line scrolling in draws the new row and
the cursor's while the rest move), and that painting only what changed --
scrolled or not -- gives the same pixels as painting everything. `font-test` covers cmap 12, the
missing-glyph flag, the vendored face, and the cache under pressure (pages
fill, the oldest is emptied, a glyph used every frame is never evicted or
overwritten); `backend-test` covers image damage in the scene renderer.
`tools/terminal_render_smoke.py` runs `ttyprobe --glyphs` (a
Powerlevel10k-style framed prompt, frames, blocks, braille, icons) and
`ttyprobe --bench` in the real Terminal.

`make terminal-screentext-test` (under ASan/UBSan) covers logical lines
across wraps; word, line, character and rectangle selection and their text;
a selection kept while its rows scroll into the scrollback; find backward and
forward, across a wrap, into the scrollback, with UTF-8 needles; and links --
URLs with brackets and trailing punctuation, `mailto:`, `www.`, OSC 8 runs, a
URL over three rows. `tools/terminal_tabs_smoke.py` drives the real window:
two tabs with their own shells, a double click, a triple click and a drag,
Copy and Paste, find into the scrollback, Super over a link and a Super-click
opening it in Files, the menu, zoom, and Super+W closing a tab -- its shell
gone and its PTY freed.

`tools/terminal_xterm_smoke.py` runs `ttyprobe --xterm` in the real Terminal:
a card of attributes, underline styles and colours, the 16- and 256-colour
palettes, truecolour, wide cells and a link, checked cell by cell through the
probe; seven questions programs ask the terminal (DA1, DA2, XTVERSION, DECRQM,
OSC 10, XTGETTCAP, `CSI 18 t`), each answer checked by the program; left,
right and wheel events and focus out and back, reported to the program; and a
long line and the scrollback re-wrapped when the window narrows.

### Golden streams

`tests/tty/golden/` holds what real programs write and what a mature emulator
shows for it: a p10k-themed zsh session, vim (mid-edit and after `:q!`),
`less -R`, htop, `git log --graph --stat`, an SGR matrix (attributes,
underline styles, 16/256/RGB colours in both separator forms), Unicode (wide,
combining, emoji, box drawing, powerline) and the OSC family (titles, OSC 7,
hyperlinks, 133 marks, colour queries, DCS/PM). `tools/record_golden.py`
records each one on a Mac: the program runs inside a private tmux server
(TERM=xterm-256color) under `script`, is frozen with SIGSTOP at its moment so
nothing more is written, and tmux's screen is captured with its attributes,
cursor, screen mode and, where set, title. The recorder replaces the host in
OSC 7 payloads (consumed, never shown) and refuses to keep a recording that
contains the user's name, host or e-mail; the p10k session runs with an empty
history file and oh-my-zsh's titles off.

`make terminal-golden-test` replays every stream through the screen model
(`tests/tty/vt_dump.c`) and `tools/golden_terminal.py` compares the shown
screen with tmux's cell by cell: text, colours resolved through the model's
palette, attributes, cursor, visibility, screen and title.
`tests/tty/golden/xfail.txt` names what the model is known to get wrong, with
the reason; a listed case that starts passing fails the run until it is taken
off, so the list only shrinks. It is empty: all nine cases match tmux.

### Terminal probe

The acceptance tools never read Terminal's internals. Terminal keeps one
fixed-layout symbol, `g_term_probe` (`user/apps/term/term_probe.h`), current
whenever the screen changes: prompt state, size, cursor, cwd, the local input
line, the shown and primary screens as UTF-8, and every shown cell's colours
and attributes. `tools/native_terminal.py` reads it through GDB, refuses an
unknown magic or version, and rereads a probe caught mid-update (its serial is
odd while Terminal writes it). `ttyprobe --bench [MiB]` writes coloured lines
as fast as the terminal drains them and reports KiB/s; the probe's frame count
says how many repaints that took.

The graphical screen acceptance launches native `ttyprobe --screen` in raw mode.
It checks application-cursor selection, confined log scrolling, Unicode/colour
cells and actual selection pixels, cursor-position reply round trips, live
resizing, multiline bracketed paste from Editor, plain screen text copied back
to Editor, alternate-screen restoration and safe prompt paste refusal. It then
doubles clipboard text through Editor commands and pastes more than 8 KiB into
Note++ to exercise its separate application hook. Read-only
GDB inspection observes state; all graphical actions use real QMP input.

## Remaining milestones

The [process compatibility layer](PROCESS_COMPATIBILITY.md) now supplies real
descriptor pipes, actual-PID waits, retained exit status and bounded
`posix_spawn`/`posix_spawnp` with ordered actions, close-on-exec and independent
starting-directory context. A real multi-child POSIX pipeline witness passes
inside Terminal. The [initial sh port](POSIX_SHELL.md) now runs scripts and
interactive pipelines. [Shared descriptions](FILE_DESCRIPTIONS.md) support
socket inheritance, shared status and cached append. Job control remains open.

1. **Unix shell compatibility:** expand the initial sh port's language and
   utility coverage, then assess an interactive shell such as zsh. Preserve
   the native structured shell alongside it.
2. **Job control:** process groups, controlling-terminal rules, Ctrl+Z/`fg`/`bg`
   across full pipelines, terminal-access checks and resize notifications. The
   current per-device target is a native handle delegation, not POSIX job control
   or a general signal implementation. `SIGWINCH`, PTY pathname allocation and
   `posix_openpt`/`grantpt`/`unlockpt` are not implemented by this milestone.
3. **Programming workflows:** broader command-line utilities/build tools, Python
   subprocess integration, complete Git transport workflows and packaged SDKs.
4. **Engineering workflows:** SSH, USB serial devices, debug probes and firmware
   flashing. Driver and physical-device validation remain separate requirements.

The existing CPython/Git/compiler ports do not imply their entire upstream
feature sets work. The next priority is Unix process/libc compatibility measured
against a small POSIX shell, followed by job control and a mature interactive
shell port.
