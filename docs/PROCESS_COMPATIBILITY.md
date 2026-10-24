# Pipes and child status for shell ports

EmbLinkOS uses explicit spawn actions and parent-owned child handles. The newlib
adapter now supplies real `pipe()`, `waitpid()` and `wait()` on that model. This
is a process compatibility foundation, not a ported POSIX shell or fork/exec.

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

`F_GETFL` now reports the actual access mode and nonblocking state, rather than
claiming every descriptor is read/write. Native pipe object-handle operations
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

When all slots are occupied, spawn returns `EMFILE`. The unpublished child is
detached and terminated through the orphan cleanup path, rather than leaving an
unreachable zombie in the parent. Existing native launchers keep their previous
reclamation policy unless they explicitly request retention.

A terminal child normally also needs three `INHERIT_FD` actions for its standard
input/output/error. Retention does not implicitly copy descriptors, environment,
namespace grants or any other authority.

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
The current guest run passes 203/203 checks twice via that wrapper and once
inside Terminal. Legacy handle reclamation, scheduler waits, 108 PTY checks,
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

`fork`, in-place `exec`, a complete `posix_spawn`/`posix_spawnp` adapter and a Unix
shell port remain open. The next adapter must translate ordered file actions,
select inherited descriptors and carry the starting directory independently of
the child's environment. The native default environment remains explicit.

Process groups, controlling-terminal rules, stopped/continued statuses and
resize signals remain separate work. Group selectors (`pid == 0` or `< -1`)
return `ENOSYS`; options other than `WNOHANG` return `EINVAL` without consuming
a result. General signal delivery remains unsupported.

Descriptor status flags are still per entry: changing `O_NONBLOCK` after `dup`
does not update its other copies. Mutable flags beyond nonblocking and a full
close-on-exec inheritance policy still need work before broader POSIX claims.
The native open operation still reserves standard descriptor slots; the new
pipe allocation explicitly supports those slots when free. These limits do not
change the tested native spawn and structured-shell contracts.
