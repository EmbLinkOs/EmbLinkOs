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

1. **File-backed `mmap`, sharing the page object.** This is the payoff for
   building the cache as an object rather than a cache: mapping a file is
   handing the process the pages it already holds. No copy, no coherence
   problem, no second implementation. It also makes `MAP_SHARED` meaningful.
2. **Demand paging and a shared zero page.** Mappings are eagerly allocated and
   zeroed today, so a large one costs its full size immediately. The VMA list
   is already the record that can answer "not mapped yet, and legitimately
   yours" — which is half of what a fault handler needs and the reason the list
   exists.
3. **Copy-on-write.** Falls out of 2 plus a per-page refcount, and is what
   makes a cheap `spawn` of a large process possible.
4. **A metadata cache.** Every `open`, `stat` and `SEEK_END` is a B-tree walk
   that reads the device. `test pagecache` deliberately keeps the seek outside
   its measured window rather than hide this. Largest single win left in I/O.
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
2. **Read-ahead**, driven by the object's own access history.
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

1. **Let user threads block on the timer queue.** They cannot today: one woken
   that way corrupts its resume on aarch64. Bisected, with a repro in
   `docs/TODO.md`. Until it is fixed, a sleeping *app* keeps a core out of idle.
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
2. **A futex-shaped primitive.** Userland has no way to block on a lock; every
   contended lock in ring 3 spins today.
3. **Per-core run queues.** One global lock guards every scheduling decision.
   It is correct and it will not scale past a handful of cores.
4. **Accounting**: per-process CPU time, so scheduling policy can be argued
   from numbers.
5. **Deadline or reservation scheduling** for the compositor and audio, which
   are the two things whose lateness is immediately visible and audible.

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
