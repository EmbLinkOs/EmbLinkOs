# Terminal sessions and the development shell

EmbLinkOS now has private pseudo-terminal pairs. Terminal launches the native
shell on a terminal slave; programs inherit that slave through standard input,
output and error. The emulator owns the master. The emulator now supplies a real VT screen model.
This does not claim that zsh or arbitrary Unix applications run here.

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

The bounded implementation supports 16 pairs, 4096 queued input cells and
4096 output bytes per pair. Canonical lines retain up to 1023 bytes plus the
newline; further line bytes are discarded until an editing/termination key.
Queue pressure returns partial writes or `EAGAIN`, or sleeps for blocking I/O.
Canonical editing operates on bytes; Unicode-aware erase is still pending.

## Terminal application

At the native shell prompt, Terminal keeps its local history, completion and
caret editing. Enter submits the command and changes to direct input until
the shell publishes its next prompt. An interactive program can then read a
canonical line, disable echo, or select raw mode. Navigation keys use ANSI
sequences on this direct path; captured modifiers keep Ctrl-letter bytes from
being mistaken for internal navigation codes. Escape is delivered to the
program. Ctrl+D at an empty shell prompt exits the shell.

The emulator master is nonblocking, with a bounded pending-input queue so a
slow program cannot block the UI thread. Output draining has a per-iteration
budget so continuous output also yields to rendering. Overflow is reported;
input is not silently discarded. Closing the emulator closes the master before collecting
its shell. Appearance and display scaling follow the Contour desktop.

## Screen model and interactions

`user/terminal/vt.c` is a UI-independent streaming parser and cell model. The
renderer paints fixed-width glyphs into retained bitmaps and redraws when screen,
input, size or appearance changes. Logical row rotation keeps ordinary scrolling
from copying an entire screen. Output work is bounded per event-loop iteration.

Implemented screen operations include:

- Cursor positioning, save/restore, pending autowrap, tabs, erasure, character
  insertion/deletion, line insertion/deletion and scroll regions with origin mode.
- Separate primary and alternate screens (`47`, `1047`, `1049`), cursor visibility
  and static outline/bar/underline styles. Alternate output never enters history.
- Normal/bright ANSI, 256-colour and RGB SGR, bold, dim, underline, inverse,
  conceal and strike. Concealed cells are also masked when copying text.
- Streaming UTF-8 with replacement for malformed sequences, basic wide cells,
  up to two combining marks per cell and DEC line-drawing characters.
- Up to 220 columns, 60 rows and 300 primary history rows. Streaming primary
  text reflows on resize; coordinate-addressed screens retain coordinates and
  clip/pad so applications can redraw after querying their new dimensions.
- Device status/cursor-position replies, primary device attributes, application
  cursor keys and bracketed paste. Unknown bounded OSC/DCS/APC/PM payloads are
  consumed without appearing as output. OSC clipboard writes are ignored.

**Copy screen** exports the visible grid as UTF-8 text, without terminal escape
sequences. It includes the local prompt when visible. **Paste** removes unsafe
control bytes. At the native prompt, line breaks and tabs become spaces and an
oversized paste is refused without inserting a prefix or executing a command.
An application that explicitly enables bracketed paste receives its delimiters
and multiline content. Cmd+C/V and Ctrl+Shift+C/V operate on the screen clipboard;
ordinary Ctrl-letter input continues to the running program. Shift+PageUp/Down
and the wheel view primary history; the alternate screen has no history view.
**Clear** and the shell's `clear` builtin clear the screen and history.

Native shell integration uses opt-in `EMBLINK_PROMPT=1` with OSC 133 prompt and
command markers, rather than overloading ordinary control bytes. The native
prompt keeps its existing 255-byte command limit and local history/completion.
The pending program-input queue holds 65,568 bytes, independently of the smaller
kernel device queues, and reports pressure in the app bar.

Shared text controls now receive a single paste command after the app hook gets
first refusal. Editors read multiline clipboard text directly; fields flatten
line breaks/tabs. This avoids overflowing the 32-key queue. Paste replaces a
selection as one undoable edit; text that cannot fit leaves the original intact.
Retained text runs explicitly clear their highlights after selection collapse.
Note++'s custom hook also bounds the full clipboard length before indexing its
buffer and accepts multiline text up to the 64 KiB clipboard limit.

### Compatibility limits

Keep `TERM=emlink`: this is a tested subset, not xterm/terminfo compatibility.
Mouse reporting, range selection, full grapheme/emoji shaping and bidirectional
layout are not implemented. Italic attributes are parsed but use the regular
face. Cursor styles stay static without animation. Basic combining/wide-cell
support does not promise every glyph is present in the installed fonts.

The current keyboard byte ABI overlaps some navigation and Ctrl-letter codes.
Captured modifiers distinguish unmodified arrows from Ctrl-letter input, but
Ctrl+arrow identity remains ambiguous. Shift/Alt arrow encoding is supported;
complete modified navigation requires physical key identity through the input
stack. Resize dimensions are queryable; there is no `SIGWINCH` yet.

## Installed development headers

The image ships `embk_terminal.h` in the SDK and installs the target's working
`termios.h`/`sys/ioctl.h` instead of newlib's missing-target forwarding header.
Shared `embk_*.h` declarations keep their relative `data/src/user/lib` location
beside staged kernel sources. A packaging test checks byte equality and unique
SDK paths even when upstream headers with the same names are present.

## Verification

```
make terminal-test terminal-screen-test
make shell-test declare-test em-test kit-test
# Boot the image, then use the serial debug console:
test terminal
test tty
# Graphical acceptance with QEMU, GDB and the existing cloud build environment:
python3 tools/terminal_smoke.py
python3 tools/terminal_screen_smoke.py
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
pressure, input flushing, independent sessions, blocked-read hangup, faulty
copyout rollback and rejected-spawn cleanup. `test terminal` checks that device
allocation returns to its baseline afterward.

The graphical acceptance script drives actual QMP keyboard input. `ttyprobe`
without options demonstrates canonical editing, receives one raw key without
Enter, restores cooked input and finishes on Ctrl+D. It verifies prompt
history/editing, foreground Ctrl+C, shell-loop interruption and independent
Terminal windows, and records real screenshots.

The screen-model host suite covers cursor/edit operations, colours, margins,
primary/alternate isolation, streaming Unicode, wide-cell resize, replies, key
modes, split-input equivalence and randomized parser/resize invariants. Run it
with AddressSanitizer/UBSan by compiling `vt.c` and `tests/tty/vt_test.c` with
`-Iuser/terminal -fsanitize=address,undefined -fno-omit-frame-pointer`.

The graphical screen acceptance launches native `ttyprobe --screen` in raw mode.
It checks application-cursor selection, confined log scrolling, Unicode/colour
cells and actual selection pixels, cursor-position reply round trips, live
resizing, multiline bracketed paste from Editor, plain screen text copied back
to Editor, alternate-screen restoration and safe prompt paste refusal. It then
doubles clipboard text through Editor commands and pastes more than 8 KiB into
Note++ to exercise its separate application hook. Read-only
GDB inspection observes state; all graphical actions use real QMP input.

## Remaining milestones

1. **Unix shell compatibility:** supply the missing process/libc semantics for
   a small POSIX shell, prove scripts and pipelines, then port an interactive
   shell such as zsh. Preserve the native structured shell alongside it.
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
