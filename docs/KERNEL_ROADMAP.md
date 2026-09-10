# The kernel we are actually building

EmbLinkOS is not a Unix clone and is not trying to be. POSIX appears at the
edges — in `user/lib`, so that ported software compiles — and stops there. The
kernel underneath is free to be a better design than the ones it borrows a
vocabulary from, and in several places it already is.

This document is the plan for the rest of it. It is organised by subsystem, and
every item says what exists, what does not, and why. Nothing here is a wish:
each stage names the thing it unlocks, because a subsystem built before anything
needs it is a subsystem that will be wrong in ways nobody can see yet.

`docs/TODO.md` is the detailed ledger; this is the shape.

---

## The two rules everything here is held to

**A capability is real or it is refused by name.** Not approximated, not
stubbed to return success. `mmap` of a file returns `ENODEV` because there is
no page cache to back it — not anonymous zeroes that look like the file until
the moment they matter. `MAP_SHARED` is `ENOTSUP` rather than quietly private.
Suspend-to-RAM is `ENOSYS` rather than a halt that looks like sleep. Every one
of those is a bug the caller finds at the call instead of a long way from it.

**A claim is measured or it is not made.** "The cache is fast" is not a claim;
`512 writes of 64 bytes -> 0 page writebacks during the loop` is. "The pages
came back" is not a claim; `free pages 128281 -> 128262 -> 128281` is. The
measurements live in the acceptance suites (`make ARCH=aarch64 test-arm64-boot`,
`test pagecache`, `test mmap`, `posixdemo`) so that a regression is a failing
test rather than a slow afternoon.

---

## Memory

The model is Mach's, not Windows': **one object owns a range of pages, and
every consumer of those bytes goes through it.** There is no block cache under
the filesystem and a separate page cache over it — two copies of the same bytes
that can disagree, where a program that `mmap`s a file and one that `read`s it
see different data and neither is wrong on its own terms.

**Built.** `struct vm_object` (`kernel/mm/vm_object.h`) owns a file's resident
pages; identity is the *file*, so two independent opens share one set of pages
and one authoritative size. Write-back with a bounded interval, real `fsync`,
a global LRU reclaimer that yields memory before an allocation has to fail.
`mmap`/`munmap`/`mprotect` over a per-process VMA list, W^X enforced, page
tables reclaimed exactly. Per-process address spaces, ASIDs on ARM, cross-core
TLB shootdown.

**Next, in order:**

1. ~~**File-backed `mmap`, sharing the page object.**~~ **Done**, and it was
   the payoff it promised: mapping a file hands the process the pages the cache
   already holds. A `MAP_SHARED` write is visible to `read()` on another
   descriptor *immediately* — no `fsync`, no `msync` — because there is only
   one set of pages. `MAP_PRIVATE` maps them read-only and **copies on write**,
   so N readers share one copy and only a writer pays.
   - Still missing: `msync` (writeback already covers durability, but a caller
     asking for it by name gets `ENOSYS`), and `MAP_SHARED` *anonymous* memory,
   which would need a shared anonymous object and has no consumer without
   `fork`.
2. ~~**Demand paging.**~~ **Done.** `mmap` allocates nothing; a page appears on
   first touch, and the fault that puts it there is *resolved* instead of
   reported. `1 GiB reserved for 0 pages, then 3 touched for 8 more (eager
   would be 262144)`. That is the difference between address space and memory
   being one resource and being two.
   - Still to come: a **shared zero page** (a read of an untouched page could
     map one copy-on-write instead of allocating), and **copy-on-write** proper.
3. ~~**Copy-on-write.**~~ **Done for file mappings** — `MAP_PRIVATE` maps the
   cache page read-only so the first write faults, and *that* fault is
   unambiguous: a page this process had already copied would be mapped
   writable and would not fault at all.
   - Not done for **anonymous** memory, which is what a cheap `spawn` of a
     large process would need — and needs a per-page refcount, since two
     address spaces would then share a frame neither solely owns. Nothing asks
     for it while there is no `fork`.
   - A **shared zero page** (a read of untouched anonymous memory mapping one
     global zero frame read-only) is the same machinery and would make a large
     sparse read-mostly mapping nearly free.
4. ~~**A metadata cache.**~~ **Done.** A warm `stat` and a warm `open` now cost
   **zero** device reads, from 22 and 24 respectively. An inode cache that
   existed but scored 0 hits (one slot, and three call sites bypassing it), a
   name cache for the other descent per path component, and a `stat` that
   stopped walking every extent to re-derive a size the inode already held.
5. **Compression before eviction**, macOS-style, once there is a reason: a
   compressed page is faster to recover than a re-read and costs no device.
   Sequenced after 1–4 because it is only worth it when the cache is under real
   pressure, which needs real workloads.

## Storage

**Built.** A neutral block layer, EMBKFS (copy-on-write, checksummed, with real
transactions), FAT32, partitions, AHCI/ATA/virtio-blk/USB storage, and a VFS
that resolves paths one component at a time so mount points compose.

**Next:**

1. **I/O scheduling and depth.** Requests go to the device one at a time. A
   queue with merging and a depth greater than one is most of the gap between
   this and a modern storage stack, and the block layer already counts what it
   would be judged on.
2. ~~**Read-ahead.**~~ **Measured, and not needed.** `test readahead` reads a
   527 KiB file cold, 4 KiB at a time: **15 device reads, 552 KiB moved, 73
   blocks per request**. EMBKFS already fetches whole extents and its
   read cache serves 131 of the 132 pages, so a read-ahead window in the page
   cache would add machinery and change nothing. The test stays as a regression
   guard — if that read path ever changes, it will say so.
3. **Write barriers and a flush op**, so `fsync` can be a device-level flush
   and not only a "the filesystem has it" flush. EMBKFS's transactions want
   the ordering guarantee, not just the write.
4. **A volume/RAID layer** under the filesystem — the block layer's
   indirection already has the right shape for it.
5. **Quotas and reservations**, which need 1 to be meaningful.

## Power

**Built.** Shutdown and reboot on both architectures, orderly (the page cache
is flushed first). Per-core idle residency, measured continuously — 97% on an
idle four-core desktop. A power-supply driver interface with an honest "this
machine has no battery" when nothing registers. A real timer wait queue, so the
kernel's own periodic threads sleep instead of spinning.

**Next:**

1. ~~**Let user threads block on the timer queue.**~~ **Done** — and it was a
   scheduler bug, not a user-thread one. `schedule_locked()` could return
   without switching while the thread was already marked `BLOCKED` and queued,
   so another core could dispatch a thread that had never saved a context. A
   sleeping app now blocks instead of spinning.
2. **Tickless idle.** A halted core still wakes 100 times a second to find it
   has nothing to do. The timer queue is what makes "when is the next thing
   due?" answerable, which is the prerequisite.
3. **Per-device power states.** Idle a disk, blank a display, quiesce a NIC.
4. **An AML interpreter** — the honest blocker for ACPI battery reporting and
   for a general x86 power-off on real hardware.
5. **Suspend-to-RAM**, last, because it needs 3 to be true of every driver.

## Scheduling and processes

**Built.** Preemptive SMP across four cores on both architectures, priorities,
per-core idle backstops, cancellation as a polite first-class mechanism (no
signals injected into user control flow), spawn with explicit file actions and
capabilities rather than fork/exec.

**Next:**

1. **Fix the user-thread resume bug above.** It gates the timer queue, which
   gates tickless idle, which gates any real power policy.
2. ~~**A futex-shaped primitive.**~~ **Done.** `SYS_futex` (WAIT/WAKE, keyed on
   the *physical* address so a shared file mapping works for free) and an
   `embk_mutex` whose uncontended path is one atomic compare-and-swap and never
   enters the kernel. Four threads, 2000 increments each, exact total, with the
   kernel's own counters proving the slow path was taken.
   - No condition variables, no read/write locks, no `pthread_*` veneer yet.
   - No **timed** wait on a futex yet — `sched_sleep_ms` now works for user
     threads, so this is a small addition rather than a blocked one.
3. **Per-core run queues.** One global lock guards every scheduling decision.
   It is correct and it will not scale past a handful of cores.
4. **Accounting**: per-process CPU time, so scheduling policy can be argued
   from numbers.
5. **Deadline or reservation scheduling** for the compositor and audio, which
   are the two things whose lateness is immediately visible and audible.
6. **Job control**, which is a scheduling and a signalling problem at once:
   backgrounding a pipeline, listing what is running, bringing one to the
   foreground, and interrupting it. Cancellation already exists as a polite
   sticky flag — what is missing is the shell-side notion of a *job* and a way
   to route the console's interrupt at one. See "What a user can actually do"
   above; this is item 1 on that list.

## What a user can actually do

A kernel subsystem is not a capability. This section is the other axis, and it
is the one to judge the project by: not "does it have a page cache" but "can a
person sit down at this machine and get something done".

**What is real today.** A graphical desktop with a compositor, windows, a dock
and a top bar. A file manager, a text editor, a terminal, a settings app, a
photo viewer, an MP3 player. A web engine with CSS, cookies and charset
handling. A package manager that installs, verifies, updates and rolls back. A
C compiler on the machine, a JavaScript engine, and `git clone`/`git push` over
real TLS. Networking down to the Ethernet frame. A structured shell that — as
of this commit — is a programming language.

**What actually blocks a user**, in the order it hurts:

1. ~~**You cannot run two things at once from a shell, or stop one.**~~
   **Done.** `&` backgrounds any statement (including a builtin pipeline, by
   spawning another shell to run its source), `jobs` is a composable table,
   `fg N` waits, and **Ctrl-C interrupts a running script** — which needed a
   new kernel channel: a *counted, clearable* interrupt alongside sticky
   cancellation, because a shell that routed ^C at itself under the old model
   took one keystroke and could never read a line again. `while` no longer
   needs its iteration ceiling.
   - Still missing: `^Z`/`bg` (suspend and resume, which needs the kernel to
     be able to *stop* a process rather than only interrupt or cancel it), and
     interrupting a background job rather than only the foreground.
2. ~~**No globbing.**~~ **Done.** `glob "*.c"` returns a *table* with ls's
   columns, so it composes with every transform already written, and rows
   carry `path` so a loop can act on them. `*` does not cross `/`; a pattern
   that matches nothing is an empty table rather than the pattern passed
   through as a filename.
   - Still missing: recursive `**`, and brace expansion.
3. ~~**No symlinks.**~~ **Done**, and the on-disk format needed no change
   after all: EMBKFS already had `S_IFLNK`, `DT_LNK`, and a `make_object` that
   mapped one to the other. A link is an ordinary object whose *content* is the
   target text. What was missing was everything above it — `symlink`/`readlink`
   ops, following in the path walker with `ELOOP`, `lstat` as a genuinely
   separate call, and `ln`/`readlink` in the shell.
   - Symlink creation is **two commits**, not one: the object then its target.
     A crash between them leaves an empty link, which the walker refuses. One
     transaction needs `make_object` to carry initial content.
   - No hard links, and no `O_NOFOLLOW` on open.
4. **Commands cannot be passed as values.** `def` makes a name, not a value, so
   the shell has no `each`-over-a-command and no way to write a higher-order
   pipeline stage. Less urgent than it was: `for f in $(glob "*.tmp") { ... }`
   covers most of what `each` would have been for.
5. **One human.** Authority is *not* the gap here, and an earlier version of
   this list said it was, which was wrong. Permission in this OS is
   object-based and it **is** enforced, in three layers:
   - **Namespaces** — a process resolves paths only from roots it was *handed*.
     An unbound prefix is `ENOENT` (absence, not "denied"), `..` can never climb
     above a binding root, and a read-only binding refuses writes.
   - **Handles** — unforgeable references to a specific object. An operation on
     a handle is handle-scoped and needs no re-check.
   - **Capability classes** — gated at the *install points* where a handle is
     obtained: `open`, socket creation, surface/window creation, audio, the
     debugger. `test capgate` proves each both ways.

   `chmod` bits are recorded and never checked, and that is **deliberate**:
   they are metadata so ported software (git's `core.filemode`) behaves, not
   the authority mechanism. Nothing should consult them.

   What is genuinely missing is the *multi-user* part — one login, one person,
   no per-user ownership of anything. Which matters far less here than it would
   in a Unix, because a namespace already does what file ownership is usually
   reached for.
6. **No clipboard-grade interop between apps** beyond the system clipboard, and
   no drag and drop.
7. **No sound input, no camera, no printing, no Bluetooth, no Wi-Fi.** Each is
   a driver and a service, and each is a day when the thing above it exists.

The pattern is worth naming: items 1–4 are all *the system is not extensible or
controllable by the person using it*. That is a different problem from "the
kernel lacks a subsystem", and it is why the shell becoming a language came
before the next kernel feature. A user who can write a command has a way to
close gaps that nobody has to ship for them.

---

## Where the design deliberately departs

These are not gaps. They are choices, and they should stay choices:

- **No `fork`.** Spawn takes an explicit list of what the child receives.
  Nothing is inherited by accident, which is why `FD_CLOEXEC` is vacuously true
  here and why a namespace can actually confine a child.
- **No signals.** Cancellation is a sticky flag a blocking call returns from,
  so a process learns at a point it already checks instead of on an interrupted
  stack. There is no `SIGSEGV` handler to write, and no re-entrancy class of bug
  to have.
- **Authority is the namespace.** A process's rights are the paths it can see
  plus its capability set, not a uid compared against a mode bit.
- **Objects, not just bytes.** Shared surfaces and zero-copy windows are kernel
  objects with refcounts, not files with `ioctl`s.

The measure of whether these are right is whether they let us build things the
borrowed designs make hard. So far they have.
