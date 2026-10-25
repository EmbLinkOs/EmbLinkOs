# Shared file descriptions and socket ownership

A descriptor is a process-local number. Duplication and explicit spawn transfer
now retain one shared description: access mode, append/nonblocking status,
socket identity and the lock for a regular file's cursor. `FD_CLOEXEC` remains
per descriptor. A fresh open has its own description; the two pipe endpoints
and the two PTY endpoints also have independent status.

## Status and lifetime

`F_GETFL` returns the actual access mode plus supported status bits.
`F_SETFL` replaces `O_APPEND` and `O_NONBLOCK` without changing access mode;
unsupported bits fail with `EINVAL` and leave the state intact. Changes through
one duplicate, a child or a transferred descriptor are visible through its
other copies. The original native nonblocking-only setter preserves append.

The native `fcntl` syscall keeps commands 1/2 for nonblocking get/set and 4/5
for close-on-exec. Command 3 reports access in bits 0–1, nonblocking in bit 2
and append in bit 3. Command 6 sets only the mutable bits 2/3; other input bits
are rejected. `embk_fcntl_get_status` and `embk_fcntl_set_status` expose that
wire contract. Newlib translates its own `O_*` values. No syscall or capability
IDs were added.

TCP/UDP socket creation, connection/listener identity and binding state survive
duplication. A child closing its inherited copy, including exit or forced
termination, leaves other owners usable. The last ordinary release closes the
socket; final process reaping uses the existing nonblocking abort path.
`posix_spawn` now supports socket snapshots and ordered dup/close actions;
close-on-exec exclusions still apply.

Blocking operations borrow a resolved backing before releasing the descriptor
table lock and return that reference on every exit. Read/write retain it across
all syscall bounce chunks, so another thread can close the number and reuse it
without redirecting the pending operation. Explicit socket operations and
metadata operations follow the same lifetime rule. A pending operation may
therefore finish after its descriptor number has been closed.

PTY loans pin storage separately from endpoint ownership. Closing the last
master still hangs up its peer immediately; an I/O loan does not keep a phantom
master alive. Native deferred termination lets an in-flight syscall unwind and
release its loan before its thread is reaped.

Creation and failure cleanup cover every backing, including console and null
descriptors. The native witness checks the shared-description, pipe and PTY
allocation baselines after exit.

## Cursor and append behavior

Read, write and seek serialize on the shared regular-file cursor. Independent
opens share cached contents while keeping separate cursors and status flags.
For append, the cache chooses EOF and performs the write under its existing
write lock, making EOF selection atomic against other cached writes, including
writers using different opens. The new cursor is the end of that write.
An append open starts with cursor zero, and seeking before an append write does
not turn it into an overwrite. Zero-length writes leave the cursor alone.

This has explicit limits: kernel writes use bounce chunks of at most 64 KiB,
which may be smaller under allocation pressure. Append selection is atomic per
chunk; larger syscall writes can interleave chunks. A running process gets
`ENOSYS` for append on a filesystem without a cache/atomic append primitive,
rather than a racy stat-then-write approximation. Ordering with truncation and
mapped writes is not a complete POSIX atomicity guarantee.

Access checks use the backing's mode, including reused descriptors 0/1/2.
A readable file redirected to descriptor 1 can be read, while a readonly file
cannot be written or truncated.

## Network details

Nonblocking UDP read/recvfrom returns `EAGAIN` on an empty queue, and UDP poll
reports queued data. Nonblocking TCP accept returns `EAGAIN` with no queued
peer; listener readiness reflects its accept queue. Accepted connections receive
their own description before the descriptor is published.

POSIX `sendto` returns the number of bytes sent, including zero for an empty
datagram. Native send retains its zero-on-success contract. Datagrams larger
than 1472 bytes return `EMSGSIZE`; the full-width length is checked before
conversion, so oversized input cannot wrap or be silently truncated.
The inline native sockets convenience shim also returns the successful byte
count, retaining its existing negative-native-error convention.

The network stack remains the existing bounded IPv4 implementation. This work
does not add process groups, general signals, fork/exec, Unix-domain sockets,
or complete BSD socket options and nonblocking-send semantics. Physical Wi-Fi
and driver acceptance remain separate from these local software fixtures.

## Verification

```sh
make fd-sharing-test
make process-compat-test terminal-test terminal-screen-test shell-test
make posixsh-test posixsh-sanitize-test
# Native serial console:
test fd-sharing
# Local echo fixtures and full desktop acceptance:
python3 tools/fd_shared_smoke.py
python3 tools/integration_smoke.py
```

The production `fd.c` host test uses a scheduler/network shim to hold a read
behind a condition-variable barrier. It exercises 128 deterministic close/reuse
races, allocation rollback, flags, close-on-exec, spawn copies and final reaping
under ASan/UBSan with leak detection. The GCC target disables ASan global
registration so the linker can discard unrelated backing tables; heap and
stack address checks and UBSan remain enabled. This does not substitute for
native scheduler or network testing.

The native probe runs concurrent shared-cursor writers/readers and independent
append writers, checks complete unique records, tests flags across every
backing, and verifies UDP port lifetime and TCP listener identity. The local
TCP/UDP echo harness checks parent/child traffic after exit and termination,
eight pending-read close/reuse cases per invocation, empty and oversized
datagrams, and exact fixture traffic. It repeats the probe inside Terminal and
returns through `sh` to the native shell. Screenshots use original QMP pixels;
no physical networking result is inferred from them.

The delivered run passes **223/223 checks** twice through the serial wrapper
and again inside Terminal, plus **83/83 network checks** through serial and
through interactive `sh`. The two fixture runs exchange exactly 192 TCP bytes
and four UDP datagrams (two empty); oversized packets do not reach the server.
The description/pipe/PTY allocation baselines are restored. Spawn (360), process
(203), PTY (108) and shell (338) regressions pass, as does the wider integration
batch including owned sync fixtures, session cleanup and kill-I/O. The x86 image
builds/boots; the AArch64 kernel compiles/links without a new userspace runtime
claim. The [gallery](media/laptop-progress/README.md) records the native captures.
