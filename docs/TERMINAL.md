# Terminal sessions and the development shell

EmbLinkOS now has private pseudo-terminal pairs. Terminal launches the native
shell on a terminal slave; programs inherit that slave through standard input,
output and error. The emulator owns the master. This is the first development
shell milestone, not a claim that zsh or arbitrary Unix applications run here.

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
its shell. Appearance, monospace rendering and transcript reflow remain shared
with the Contour desktop.

## Installed development headers

The image ships `embk_terminal.h` in the SDK and installs the target's working
`termios.h`/`sys/ioctl.h` instead of newlib's missing-target forwarding header.
Shared `embk_*.h` declarations keep their relative `data/src/user/lib` location
beside staged kernel sources. A packaging test checks byte equality and unique
SDK paths even when upstream headers with the same names are present.

## Verification

```
make terminal-test
make shell-test
# Boot the image, then use the serial debug console:
test terminal
test tty
# Graphical acceptance with QEMU, GDB and the existing cloud build environment:
python3 tools/terminal_smoke.py
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

## Remaining milestones

1. **VT screen emulation:** cursor addressing, erasure, scroll regions, alternate
   screens, cursor visibility, Unicode cell widths, selection and paste. Output
   currently supports transcript text and SGR colours only. Keep `TERM=emlink`;
   advertising `xterm-256color` before implementing it would mislead applications.
2. **Unix shell compatibility:** supply the missing process/libc semantics for
   a small POSIX shell, prove scripts and pipelines, then port an interactive
   shell such as zsh. Preserve the native structured shell alongside it.
3. **Job control:** process groups, controlling-terminal rules, Ctrl+Z/`fg`/`bg`
   across full pipelines, terminal-access checks and resize notifications. The
   current per-device target is a native handle delegation, not POSIX job control
   or a general signal implementation. `SIGWINCH`, PTY pathname allocation and
   `posix_openpt`/`grantpt`/`unlockpt` are not implemented by this milestone.
4. **Programming workflows:** broader command-line utilities/build tools, Python
   subprocess integration, complete Git transport workflows and packaged SDKs.
5. **Engineering workflows:** SSH, USB serial devices, debug probes and firmware
   flashing. Driver and physical-device validation remain separate requirements.

The existing CPython/Git/compiler ports do not imply their entire upstream
feature sets work. The next priority is screen emulation and compatibility
measured against real applications, followed by a mature shell port.
