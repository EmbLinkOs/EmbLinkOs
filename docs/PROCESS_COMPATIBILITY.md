# Pipes and child status for shell ports

EmbLinkOS uses explicit spawn actions and parent-owned child handles. The newlib
adapter supplies real `pipe()`, `waitpid()`, `wait()`, `posix_spawn()` and
`posix_spawnp()` on that model. This is a bounded process compatibility layer,
underlying the [bounded sh port](POSIX_SHELL.md), without fork/exec.

## Descriptor pipes

`pipe(fds)` creates a read descriptor and a write descriptor in the lowest free
slots, including closed standard-input/output slots. Both report FIFO type.
Ordinary read/write, duplication, explicit descriptor transfer, readiness and
last-writer EOF use the existing kernel pipe implementation. Failed allocation
or result copyout releases both descriptors and temporary object handles; the
caller's result array is unchanged on failure.

Descriptor operations honour `O_NONBLOCK`. Empty reads with a live writer and
writes without enough room return `EAGAIN`; empty pipes without writers return
EOF. Writes through descriptors of at most the 1024-byte pipe buffer are atomic:
they wait for the entire write to fit, or fail without inserting a prefix in
nonblocking mode. Larger writes may be partial. A missing reader returns `EPIPE`;
there is no asynchronous `SIGPIPE` delivery.

`F_GETFL` reports actual access mode plus shared append/nonblocking state;
`F_SETFL` updates supported status through all duplicates. Native pipe object-handle operations
retain their partial-write contract. Syscall 49 still returns object handles;
**new syscall 150, `pipe_fds`, returns descriptors**. An unused register in an
older one-argument syscall is not an option field. Neither call interprets one.

## Owned child results

The shared `embk_child.h` ABI defines a 16-byte result with actual PID, parent
handle, signed exit code and completion/forced-termination/retention flags.
Syscall 149, `child`, operates only on child handles the caller already owns.
Choosing a PID does not grant authority over another process.

- `embk_child_control(handle, EMBK_CHILD_INFO, 0, &result)` inspects without
  consuming. A live child's exit-code field is zero; `EMBK_CHILD_DONE` indicates
  a collectible result.
- `EMBK_CHILD_WAIT_HANDLE` selects a handle; `EMBK_CHILD_WAIT_PID` selects an
  actual PID. `EMBK_CHILD_ANY` (`-1`) selects any owned child for a wait.
- These native waits return 1 after copying a result, 0 for `NOHANG` when no
  selected child is ready, or a negative native error. A negative exit code is
  data, so `exit(-10)` cannot be mistaken for the native `ECHILD` error.
- Newlib's `waitpid(pid, status, 0)` blocks in the kernel. `wait()` and
  `waitpid(-1, ...)` collect whichever owned child finishes; neither polls in
  userspace. `WNOHANG` returns zero without changing the status destination when
  nothing is ready. A null status pointer discards the status.
- Copyout precedes removal from the zombie list and release of the handle. An
  invalid destination returns `EFAULT`, leaving the result available to retry.
  Competing waiters collect a child exactly once; another receives `ECHILD`.
  A specific handle wait pins its PID across sleeps so slot reuse cannot redirect
  that wait to a different child.
- Every child completion wakes all waiters on the shared parent queue, so a
  thread waiting for a different child cannot consume the only wakeup.
  Cancellation refuses a pending block and becomes `EINTR` in the libc adapter;
  it preserves the handle. An already-completed result wins over cancellation.

Native normal exits become POSIX status `(code & 255) << 8`. An actual forced
termination is recorded separately and maps to `WIFSIGNALED` / `SIGKILL` (9).
Normal `exit(-1)` is exit status 255, not a forced termination. This single
disposition mapping does not implement general signals or `SIGCHLD` delivery.

## Retaining status

Native launchers may intentionally discard old completed child handles when
their table fills. A shell must retain statuses until it collects them. Include
`EMBK_SPAWN_ACTION_KEEP_STATUS` (10) in the native spawn actions to request this.
The handle and its retention policy are published together under the scheduler
lock. Pressure reclamation skips retained statuses. The bound remains 32 handles
per process, counting both live children and retained completions.

Spawn reserves a handle before creating a child or applying file actions. A
reservation has PID zero, is retained against pressure reclamation, and cannot
be resolved as a child. Construction failure frees the reservation. When all
slots are occupied, spawn returns `EMFILE` before running another program. The
completed child is linked into the parent's live-child list under the scheduler
lock only after construction succeeds; failed constructions leave no stale
list entry. Existing native launchers keep their previous reclamation policy
unless they explicitly request retention.

A terminal child normally also needs three `INHERIT_FD` actions for its standard
input/output/error. Retention does not implicitly copy descriptors, environment,
namespace grants or any other authority.

## Spawn adapter

`posix_spawn(&pid, path, actions, attributes, argv, envp)` creates a fresh native
process and returns zero or a **newlib error number directly**. It preserves
`errno` and leaves `pid` unchanged on failure. It never replaces the caller or
pretends to fork. The explicit `envp` is passed unchanged, including an empty
or null environment. The child status is retained until an owned `waitpid` or
`wait` collects it. The 32-handle limit counts live and completed children.

Native syscall **152, `spawn_pid`**, uses the existing six-argument spawn layout
but returns the actual PID with retained handle publication. This avoids a libc
follow-up lookup racing another thread's wait. The original spawn syscall still
returns a handle. Neither operation changes the contract of an unused register.

The adapter explicitly requests a snapshot of the caller's open descriptors.
Regular files share their open description/cursor; pipes and PTYs take proper
endpoint references. Closed standard slots remain closed. Actions are then
applied **in insertion order on the child table**:

- `addopen` opens a copied path onto the chosen descriptor, including `/dev/null`.
- `adddup2` duplicates a descriptor from the child's current table, so chains and
  temporary-descriptor swaps work. It clears the target's close-on-exec flag,
  including `adddup2(fd, fd)`; ordinary `dup2(fd, fd)` preserves that flag.
- `addclose` closes the named child descriptor. A closed or invalid descriptor
  fails with `EBADF` before the child runs.
- `addchdir_np` changes the directory used by subsequent relative open/chdir
  actions and the executable path. Paths use the libc's existing lexical
  normalization. Neither the parent's descriptors nor its directory is changed.

`F_GETFD`, `F_SETFD` and `O_CLOEXEC` now store real per-descriptor state. Ordinary
`dup`/`F_DUPFD` clear it. After actions, the compatibility policy closes entries
still marked close-on-exec. A marked file/pipe/PTY can therefore serve as an
explicit dup source before it is closed. Legacy native explicit transfers keep
their existing inheritance policy.

The starting directory travels in a dedicated process field, read through
**syscall 151, `spawn_cwd`**, before constructors run. A null/empty environment or
a conflicting `PWD` does not change it. Legacy native launches without explicit
context retain their existing best-effort `PWD` startup behavior.

`posix_spawnp` searches the **caller's** `PATH`; the child's `envp` does not select
its executable. Empty and relative entries use the child's final starting
directory. An unset PATH uses `/system/bin:/bin`. Names containing `/` bypass
search. Missing/non-directory candidates continue searching; a remembered
`EACCES` takes precedence over exhaustion. Unrecognized executable formats fail
without a shell fallback.

Native actions 11–14 add descriptor snapshot, child-table dup2, close and cwd.
The native list now holds at most 64 actions and is copied into kernel heap
memory rather than expanding the syscall stack. The adapter reserves three
entries for its explicit policy/context, leaving **61 ordered user actions**.
Paths are at most 255 bytes; the existing 127-argument, 31-environment-entry,
1024-environment-byte and combined 4 KiB startup-page limits still apply.
Oversized requests fail without truncation. Adding an action copies its path,
and failure leaves the action list unchanged.

Attributes have initialization, destruction and stored get/set values. Only
zero attribute flags can be applied: identity, process-group, scheduler and signal
flags return `ENOTSUP` without mutation; unknown bits return `EINVAL`.
`addfchdir_np` returns `ENOTSUP` because the VFS cannot reconstruct an fd's path.

TCP/UDP descriptors now have shared lifetime and identity. Snapshots and
ordered dup/close actions preserve other owners through child exit or
termination. Close-on-exec exclusions still apply. Shared status, in-flight
I/O loans and cached append behavior are documented in
[FILE_DESCRIPTIONS.md](FILE_DESCRIPTIONS.md).

## Spawn validation

```
# Serial debug console:
test spawn-compat
# Full booted desktop acceptance:
python3 tools/spawn_compat_smoke.py
```

`spawnprobe` runs real ring-3 children through the installed libc. It checks
inherited files/pipes and descriptor flags, ordered open/dup/close chains, cursor
swaps, preserved parent state, closed stdio, action limits, directory setup
before constructors, exact environments, PATH search, failure cleanup and all
32 retained statuses. A full handle table must not execute a marker child.
Two spawned children transform 66,049 bytes through bounded descriptor pipes.
The later shared-description run passes **360/360 spawn checks** in each of two serial runs and
again inside Terminal. Fixtures live in the invoking user's home (or `/data`
for the kernel-context wrapper), so the GUI run uses the normal session namespace. The harness repeats
the serial run and executes the same program in Terminal, then checks another
shell command. It also reruns the process, PTY, legacy-handle, scheduler,
console/POSIX and channel regressions and checks pipe allocation baselines.

## Validation

```
make process-compat-test
make terminal-test terminal-screen-test shell-test
# Serial debug console in a booted image:
test process-compat
test handle reap
test sched wait
# Full native acceptance with the cloud build environment activated:
python3 tools/process_compat_smoke.py
python3 tools/integration_smoke.py
```

`processprobe` is a ring-3 witness using the installed libc and native SDK. It
checks lowest-free descriptors without assuming the shell supplies only three
fds, FIFO type, EOF, nonblocking/atomic writes, exhaustion rollback, and the
original pipe syscall with nonzero unused registers. Child tests cover actual
PIDs reported by a child, signed exits, bad-status-pointer retry, null status,
`WNOHANG`, out-of-order completion, forced termination, cancellation and competing
waiters. All 32 retained statuses survive pressure before reverse-order
collection. Two native children transform 66,049 bytes through bounded pipes.
The same program runs in Terminal, and a following command must still work.
The serial wrapper checks that live pipe allocation returns to its baseline.
Pipe/wait acceptance passed 203/203 checks twice via that wrapper and once
inside Terminal. The later spawn harness repeats all 203 checks alongside its
own child tests. Legacy handle reclamation, scheduler waits, 108 PTY checks,
console/POSIX/channel tests and the wider integration batch also pass. The x86
images build and boot; the AArch64 kernel compiles and links without a new ARM
runtime claim. Blocking wait paths restore interrupts through the architecture
helper, without interpreting an x86 flag bit in shared code.

The host suite compiles the production pipe state machine with a minimal
scheduler/handle shim. It checks deterministic byte streams, nonblocking and
atomic writes, cancellation/EOF priority and both handle-allocation failure
paths. Real sleeping, competing threads and descriptor ownership are established
by guest checks, not inferred from the shim. Host sanitizer command:

```
cc -std=c11 -Wall -Wextra -Werror -O1 -g -Ikernel \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/process/pipe_host.c -o /tmp/pipe-host
ASAN_OPTIONS=detect_leaks=1 /tmp/pipe-host
```

## Remaining compatibility work

An initial [spawn-based sh port](POSIX_SHELL.md) now uses this adapter for
external programs and isolated shell workers. `fork`, in-place `exec` and
broader process compatibility remain open. The spawn adapter above is a bounded implementation with explicit
limits, not a claim of complete POSIX process semantics.

Process groups, controlling-terminal rules, stopped/continued statuses and
resize signals remain separate work. Group selectors (`pid == 0` or `< -1`)
return `ENOSYS`; options other than `WNOHANG` return `EINVAL` without consuming
a result. General signal delivery remains unsupported.

`O_NONBLOCK` and `O_APPEND` now belong to shared descriptions; socket
inheritance, cursor serialization and cached per-chunk append selection work.
Writes larger than the syscall bounce chunk can interleave chunks, uncached
append needs an atomic backend primitive, and truncate/mapped-write ordering
needs further work. See the [description contract](FILE_DESCRIPTIONS.md).
Native open and pipe allocation use the lowest free slot, including closed
standard descriptors. Job control remains separate work.
