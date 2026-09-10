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
2. ~~**Tickless idle.**~~ **Done, and honestly a modest win.** A core about to
   halt now arms its timer for the next thing actually *due* rather than for
   the next 10 ms quantum. Timer interrupts on the secondaries fell from
   **~98/s to ~70/s**; system idle is unchanged at 97-98%.

   It is not lower because the floor is real work, not the tick: the page
   cache's writeback thread wakes ten times a second, apps sleep on timers, and
   a core cannot idle past the next thing genuinely due. Getting further means
   having fewer periodic wakers, not a cleverer timer.

   The interesting part was a **failed** first attempt. Waking idle cores with
   `arch_ipi_broadcast` — one thread becoming runnable interrupting every other
   core — cost *more* than the tick it replaced: idle fell from 98% to **87%**.
   A targeted `arch_ipi_send` to exactly one idle core recovered it. That
   measurement is why the targeted IPI exists on both architectures.
   - The BSP still ticks at ~100/s: its REPL polls the serial console, so it
     keeps a short arm deliberately. A UART RX interrupt would fix that.
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
3. **Per-core run queues — measured, and NOT worth building yet.**

   The policy seam exists (`process/sched.h`), so a queue-based policy would be
   a new file rather than an edit to `schedule_locked()`. What was missing was
   a reason. `test schedlock` supplies it:

   | | acquires | contended | spin cycles |
   |---|---:|---:|---:|
   | 2 s idle | 1270 | 247 (19%) | 53166 |
   | under a 4-thread load | 2554 | 43 (1%) | 12101 |

   **~0.04% of one core.** Nineteen percent *contended* sounds alarming and is
   the wrong number to read: a lock taken a few hundred times a second can be
   contended half the time and still cost nothing. What matters is the time
   spent spinning, and it is noise.

   So: not built. It would be the riskiest refactor in this kernel — the
   function where two separate real bugs have already been found — to buy four
   hundredths of one percent. Revisit when either number moves: many more
   cores, or a workload with far more scheduling events than four threads on
   one mutex.

   Two real things came out of measuring it, though:
   - **Tickless made the cores wake in lockstep.** Every idle core computes the
     same "earliest deadline" — it is a property of the system, not of the core
     asking — so all four armed for one instant and piled into the lock
     together. An *idle* machine contending harder than a busy one is the
     signature. They are skewed by core now.
   - **`sched_idle_next_ms` took the lock on every halt, purely to read.** A
     core halts thousands of times a second. The earliest deadline is cached
     now and read lock-free; acquisitions at idle fell about a third.
4. ~~**Accounting**: per-process CPU time.~~ **Done** — `ps` carries a `cpu`
   column (milliseconds), and in the shell it is an ordinary column that sorts
   and filters: `ps | sort-by cpu | last 5`. Charged at the one place a switch
   happens, so it is exact rather than sampled — a thread that blocks between
   two ticks is still billed for what it ran, which is precisely what a
   tick-sampled counter loses.

   **It immediately caught a bug in the idle accounting, and the idle numbers
   in this document were wrong.** `power` reported 99% idle on a machine whose
   CPU accounting said one process was using half a core. The CPU one was
   right: a core halts inside `power_idle_enter`'s bracket, an interrupt wakes
   it, and the handler *switches to another thread* — so the idle interval
   stayed open for the entire time that other thread ran. Every core reported
   itself idle while working. The switch path closes the interval now.

   Real figures: **~87% idle, ~12% busy**, and the two independent measurements
   agree within a couple of percent (the remainder is interrupt-handler time,
   which neither charges). The timer-interrupt *rates* quoted elsewhere were
   measured directly and are unaffected.
5. **Deadline / reservation scheduling — DONE**, and it is the first thing the
   policy seam was built for: `kernel/process/sched_deadline.c` is a new file
   and `schedule_locked()` was not touched.

   A thread declares a rate — `embk_sched_period(period_ms, budget_ms)`, "I
   must run once every 16 ms and I need about 4 ms of it". A thread holding a
   live deadline with unspent budget beats every thread without one, and among
   those the nearest deadline wins. **Past its budget it is an ordinary thread
   again** for the rest of the period — not throttled, not stopped, not
   penalised next period, just no longer urgent. That, plus admission control
   that refuses a declaration once 700 permille of the machine is promised
   away, is the whole reason this is safe to hand to ring 3 without a
   capability gate: the worst a caller can do is claim `budget/period` of a
   core ahead of its neighbours, and the sum of those claims is bounded.

   Why not priority: priority says "before everyone else, always", so two
   threads that both need a cadence starve each other, and a thread needing
   2 ms out of every 16 takes all 16. A deadline says *when*.

   **The measurement** (`test deadline`, the same binary run twice against the
   same load, once under each policy — 16 ms period, 150 periods, six threads
   that never sleep on four cores):

   | policy | worst lateness | mean | periods missed |
   |---|---|---|---|
   | round-robin | 75 / 75 / 114 ms | 19.4 / 20.8 / 30.0 ms | 70 / 78 / 94 of 150 |
   | deadline | 13 / 13 / 13 ms | 4.6 / 5.8 / 6.0 ms | **0** |

   Round-robin drops roughly half the frames of a 60 Hz loop under load. This
   drops none. **aarch64 runs the same A/B in its boot self-test** and agrees
   under both HVF and TCG: 51–55 ms worst down to 6 ms, 0 missed.

   **What it is not.** Not a real-time scheduler, and it must not be described
   as one: there is no bounded-latency interrupt path, no priority inheritance
   on the futex, and the admission test ignores blocking time. The claim is
   narrower and is the one that was measured: *under CPU contention, a thread
   that declares a rate keeps it.*

   **What the measurement then exposed.** The residual mean is not a policy
   problem at all — a sleeping thread only becomes runnable inside
   `wake_expired_locked()`, which runs inside `schedule()`, and on a machine
   with no idle core nothing re-enters `schedule()` until the next tick. The thread is late because it was not yet
   a *candidate*, not because the wrong one was picked. Fixing that means
   arming the one-shot timer for the next sleeper on busy cores too; it is
   written up in `docs/TODO.md` rather than guessed at. A preempt-on-wake IPI
   was built for this and **removed** — it moved the mean by less than the
   run-to-run noise, which is the same answer read-ahead and per-core run
   queues got.
6. **Something actually declares a cadence now**, which is what makes item 5
   more than a switch nobody flips.

   **The UI toolkit declares for the app.** `em_app_run` holds a reservation
   while an app is animating and gives it back when it stops — apps do not have
   to know this exists. Two details were got wrong first and are worth keeping
   written down:

   - *"N frames in a row" is the wrong test for animating.* A loop polling
     every 10 ms that renders 30 fps video builds one frame in three and never
     two consecutively, so a consecutive-frames rule sees video as idle. With
     that rule nothing on the desktop ever declared anything. It is a heat
     counter now: a frame adds three, an idle iteration subtracts one, held
     while ≥ 9.
   - *The budget must cover the whole active part of an iteration.* Timing only
     build-render-present measured 3–4 ms and omitted input polling and the
     present's copy, so the app declared a budget it exhausted inside its own
     frame and spent the rest of every period unprivileged — the opposite of
     the intent.

   The budget is *measured*, not guessed: the minimum of the recent
   frame costs, because wall time includes preemption and the minimum is the
   sample that suffered least. Declaring half the period "to be safe" would
   claim half a core for a 4 ms frame, and admission control would then refuse
   the third app on the desktop.

   `framepace.elf` is a real EmApp — real window, real render, real compositor
   surface — and exists because a quiet desktop never animates, so without it
   the declaration is code that is never reached.

   **What it actually reports on this host, and it is not a win.** Under TCG
   with six busy threads, one frame of a 240×120 window costs **10–14 ms of
   CPU** — more than half a 16 ms period — so the toolkit declines to declare,
   says so with the number, and backs off. That is the correct answer: an app
   that needs more than half a core continuously is not periodic at that rate,
   admission control would rightly refuse it, and granting it would deny the
   claim to an app that could keep it. No scheduler makes this machine able to
   render 60 Hz in software under that load.

   Both branches are verified: the declaring path fired (`pacing 16 ms,
   budget 4 ms`) while the cost estimate was lower, and the refusal path fires
   now with its reason printed. The frame-interval spread is *printed and not
   asserted* — with neither run declaring, any difference between them is
   run-to-run noise, and reading it as a result would be exactly the mistake
   this project keeps refusing to make.

   The obvious next step, **not taken**: an app that cannot hold its pace
   should slow to one it can and hit that evenly — 30 fps smooth beats 60 fps
   ragged. It is not done because the benefit cannot be measured on this host,
   where the spread is noise-dominated. See `docs/TODO.md`.

7. **Audio: the latency floor moved 40 ms → 15 ms.** The buffer a writer keeps
   queued *is* the delay between deciding to make a sound and the sound
   existing, and how shallow it can be is a scheduler question.

   | policy | shallowest clean runway | at 20 ms | at 15 ms |
   |---|---|---|---|
   | round-robin | 40 ms | 12–19 underruns | 22–28 underruns |
   | deadline, declaring 10/3 ms | **15 ms** | 0 | 0 |

   An underrun is the hardware's own report — the AC'97 latches "reached the
   end of the list and halted", which `ac97_set_last()` has always had to
   detect to restart the stream and simply never counted.

   Three real gaps had to be closed first, none of them test scaffolding:
   `audio_position()` (the device's actual playback position — the first
   version of the test paced off the wall clock and measured nothing, because
   the device starts when the prefill is met, not when you start writing);
   `audio_latency(ms)` (the ~170 ms prefill was a floor no scheduler could get
   under; it is the writer's choice now, floored at one timer tick); and
   per-descriptor lengths in `ac97_fill()`, which declared every descriptor a
   full page and zero-padded, making 21 ms the smallest unit of audio the
   device could be given. `make test-audio-latency` runs the sweep.

8. **Job control**, which is a scheduling and a signalling problem at once:
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
