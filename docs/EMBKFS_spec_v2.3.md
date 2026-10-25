# EMBKFS Specification — Additions (v2.2 → v2.3)

This document layers v2.3 on top of v2.0 (`EMBKFS_Specification_v2.0.pdf`),
v2.1's corrections (`EMBKFS_spec_v2.1.md`) and v2.2's feature set
(`EMBKFS_spec_v2.2.md`), following the same discipline: everything here is
exactly what the formatter (`mkfs_embkfs.py`), the independent verifier
(`verify_embkfs.py`) and the kernel reader/writer (`kernel/fs/embkfs/embkfs.c`)
actually implement, cross-checked against each other.

**v2.3 changes NO on-disk structure.** There is no new item type, no new field,
no new `feature_incompat` bit, and `version_major` stays **1**. A v2.2 reader
reads a v2.3 image and vice-versa. What changed is *semantics* (one genuinely
new atomic operation), *formatter policy* (extent sizing), and the *read path*
(which got ~140× faster). Those are worth specifying precisely because they are
what a second implementation would otherwise get subtly wrong.

Everything below was driven by real software: **git** forced the atomic rename,
**CPython** forced the read path.

---

## 1. Atomic `rename` — the one real semantic addition

`embkfs_rename()` / `embkfs_rename_path()`. Renaming over an existing name
**replaces it atomically**: the destination's old object is torn down in the
**same B-tree commit** that repoints the name. There is no instant at which the
destination is missing, and no instant at which it names both objects.

This is not gold-plating. **git's lockfile protocol depends on it**: it writes
`X.lock`, then renames it over `X`, and correctness rests on that swap being
indivisible. `git commit` does it for every ref update.

### 1.1 Why `remove` + `add` does NOT compose

The subtle part, and the reason this needs specifying. Directory entries are
keyed by name hash — `{dir_oid, DIR_ENTRY, hash(name)}` — and names that collide
on the hash **share one item** as a chain of records (v2.0 §9.2). Given a
same-key edit, the obvious implementation is "remove the record, then add the
new one".

**That is wrong**, because each of those operations builds its new item payload
from the **original** chain. Applied to one key in one transaction, the second
operation silently discards the first's result — the last writer wins and the
chain is corrupted.

So v2.3 adds an internal primitive, `embkfs_dirent_retarget_op()`, which
performs the whole edit **as a single item rewrite**: it reads the chain once,
produces the new payload once, and writes it once. Any implementation that
supports rename must do the same, or handle collision chains some other way that
is closed under composition.

### 1.2 Known limitation (deliberate, not a bug)

A rename **between two names that hash-collide inside the same directory** is
refused (`-EINVAL`) rather than done wrong. Source and destination would live in
the same chain item, and the retarget primitive edits one record at a time. The
refusal is honest and loud; the frequency is `1/2^32` per pair.

## 2. Other operations added since v2.2

| Operation | Kernel entry | Notes |
|---|---|---|
| `chmod` | `embkfs_chmod_object()` | mode-only inode update. `git init` probes `core.filemode` by chmod-ing a probe file and stat-ing it back. |
| `ftruncate` | via `vfs_fd_truncate` | grow and shrink; shrink frees whole extents. |
| real `unlink` | — | v2.2's stub was a **stale lie**: it returned success without removing anything. Fixed. |
| `/dev/null` | `FD_BACKING_NULLDEV` (fd layer) | **Not an EMBKFS object.** Special-cased in `vfs_open` before path resolution — there is no device-node type on disk, and inventing one to satisfy a name would be worse than the special case. |

## 3. Formatter policy: bounded extents (`EXTENT_MAX_BYTES = 1 MiB`)

**Not a format change** — the format has always allowed many extents per file.
This is what the formatter *chooses*, and it is spec-relevant because the old
choice made large files pathological to read.

**The problem.** mkfs used to emit **one extent per file**, and an extent's
checksum covers the **whole extent**. So nothing in a file could be served until
*all* of it had been read and CRC'd: a single 8 KB read of the 10.3 MB
`python314.zip` cost O(filesize). Cold read amplification was **443%** — the
reader "CRC'd 2 GB before byte 0".

**The fix.** `build_extent_items()` splits file data into extents of at most
**1 MiB**. Cold amplification fell to **194%**.

**Do not "improve" this to 64 KiB from first principles — it was A/B'd and it is
worse.** Measured over a real boot:

| Extent bound | device reads | of which B-tree node reads | total device time |
|---|---|---|---|
| 64 KiB | 8535 | 8336 | 10860 ms |
| **1 MiB** | **5401** | **5152** | **5886 ms** |

Smaller extents hit a lovely 101% cold amplification on file data — and cost
**85% more device time system-wide**, because **~95% of device reads on a real
workload are B-tree node reads**, and more extents means more extent items means
a bigger tree. Optimising the visible number made the system slower.

Extent lookup within a file is a **binary search** over the (sorted) extent
array, not a linear scan — with up to `filesize / 1 MiB` extents, the scan showed
up.

## 4. The read path (implementation, no format impact)

A second implementation is free to ignore all of this; it is recorded because
the numbers are the whole reason the OS can host CPython and git at all.

**36000 ms/MB → ~250 ms/MB warm.** The disk went from dominating wall-clock to
**1–4%** of it. Warm steady state: 48 requests for 3 MB, 64 KB each, **100%
amplification — zero waste**.

| Mechanism | Constant | What it fixes |
|---|---|---|
| inode cache | `embkfs_inode_cached()` | re-resolving an inode per read |
| extent cache | `embkfs_extents_cached()` | shared, cache-owned extent array |
| windowed read-ahead | `EMBKFS_WCACHE_WIN` = 64 KiB | the read-ahead the whole-object cache could not do |
| whole-object cache | `EMBKFS_RCACHE_MAX` = 8 MiB | small files; **the 10.3 MB zip got ZERO caching from it** — hence the window |
| batched device reads | `EMBKFS_IO_BATCH_BLOCKS` = 16 | per-request overhead in steady state |

**The old "request_size / 2.7 ms law" from earlier notes is WRONG.** Killed by
measurement, along with bigger block bounce buffers, overlapped ATA I/O, and the
ecache-thrash theory. Seven plans missed by optimising unmeasured layers.
**Run `test ioperf` before touching this path**, and measure the layer before
optimising it — one counter, one boot, every time.

## 5. Clarifications

- **Blocks 1..15 are FREE space and the allocator will use them.** Only block
  **0** is permanently reserved — an all-zero `block_ptr` is the "points at
  nothing" sentinel, so no real node or data block may live there. Plus the
  primary superblock (block 16) and the backup (last block). The formatter's map
  (16 = superblock, 17 = root, 18.. = leaves then data) never *claims* 1..15,
  and `embkfs_bitmap_build()` never marks them — so a file landing at block 1 is
  correct, not corruption. (It looks alarming in a hex dump. It isn't.)
- **The bitmap is always recomputed exactly from the live tree**, so the
  superblock's `free_blocks` is a **hint**, not an authority. A reader must not
  trust it.
- **The root directory is FLAT — mkfs packs no directories**, and that is now
  load-bearing rather than incidental: the on-OS C compiler finds `crt0.o`,
  `syscalls.o` and `libc.a` (6.6 MB, the largest object on the image) at `/` via
  `-L/`. The flat root *is* the path search. See [PORTS.md](PORTS.md).
- **The image is 64 MB** (`python.elf` 7.8 + stdlib zip 10.3 + `git.elf` 3.5 +
  `libc.a` 6.6 already). The budget is now a design input.
- **Single-leaf overflow is real**: enough packed objects push the metadata past
  one 4 KiB leaf, and `build_btree` auto-splits to a level-1 root. The
  hash-collision fixtures were dropped from the default image when it overflowed.

## 6. The root mirror (v2.3.1) — one block must not be the whole volume

### 6.1 Why

Every lookup passes through the tree's root node. The superblock has had a
backup at the last block since v1; the root node had none, and an HP ProBook
showed what that costs: a USB stick with one marginal flash page under block
17 — the root of a freshly written image — failed every read of it with I/O
error, and the whole root filesystem was unreadable before the first commit
could have moved the root anywhere else.

### 6.2 On disk

A **byte-identical copy of the root node** is kept at a second block, named by
a header in the superblock block at a fixed offset, past the crypto header
(200..252, v2.2 Phase 4) and the verify header (260..308, v2.2 Phase 5d):

```
offset 320: struct embk_mirror_header  (48 bytes)
    uint64_t              magic;         "EMBKMIR1" = 0x3152494D4B424D45
    struct embk_block_ptr root_mirror;   block = the mirror's block; checksum and
                                         generation = THE ROOT'S (the bytes are
                                         the root's, so they carry its tag)
    uint64_t              checksum;      CRC32C over the 40 bytes before it
```

Self-described by magic and its own checksum, **not** covered by the
superblock's checksum (which ends at 152) and announced by no feature bit: a
kernel that predates it ignores the bytes, a kernel that has it treats an
absent or invalid header as "no mirror". The backup superblock carries the
same header, because it is a copy of the whole block.

The copy's node header says `block = <root's block>`, not its own. That is
deliberate and is what a reader checks it against: the mirror is not a node of
the tree, it is the root's bytes kept somewhere else.

### 6.3 Who writes it

- **mkfs** places the copy at `total_blocks − 2`, immediately inside the backup
  superblock: the far end of the medium, a different flash page from the
  tree's first blocks on any real device. It counts as a used block.
- **Every commit** allocates a block **from the far end of the free space**
  — the allocator otherwise hands out the next free block, which on a flash
  medium is the same page as the root it is meant to survive — reads the
  just-written new root back, writes it there, names it in the superblock it
  is about to install, and — once that superblock is sealed — releases the
  previous mirror's block. A failure anywhere in staging the mirror is logged
  and the commit proceeds **without** one: the mirror is a safety net, never a
  reason a write fails.
- **Mount** marks the mirror's block used, next to the superblocks and the
  snapshot registry, so the allocator cannot hand it out.

### 6.4 Who reads it

`embkfs_read_node()`, for the root only: if the primary read fails — I/O
error, bad magic, bad self-checksum, Merkle or generation mismatch — and a
mirror is named, the mirror's block is read and validated against the **root
pointer** (self-checksum, `checksum == root.checksum`,
`generation == root.generation`, `header.block == root.block`). Success is
logged once per mount: `root node at block N could not be read (...); using
its mirror at block M from now on -- the medium is failing there`, and **from
then on the primary is not asked again** for that root block: every lookup
reads the root, and on the medium this is for each failed primary read is a
30-second timeout. A commit moves the root and the new block starts trusted.
Nothing is written back to the failing block: on the device that motivated
this, a write near that page hung the device's own firmware.

### 6.5 What it does not do

It saves the volume when the **root** dies. An interior node or a leaf with
one copy is still one copy; the files under it are lost with it. Duplicating
all metadata is the next step up and is not this.

## 7. Read-only on device failure (v2.3.1)

A device that fails a write is not asked to take another. The first write or
flush that EMBKFS issues to its block device and gets `EIO` back — a node
write, a data-extent write, the superblock write, either of the commit
barrier flushes, the snapshot registry — sets the volume read-only, exactly as
an unknown `feature_ro_compat` bit would at mount, and logs it once:

```
EMBKFS: sdb: a write to the device failed with an I/O error; the volume is
read-only from now on -- later writes fail at once instead of waiting on a
failing device
```

From then on every write entry point returns `EROFS` before touching the
device; reads are unaffected. The transaction whose write failed is rolled
back as before, so the on-disk tree is the last committed one. The first
write refused after that is logged once too --
`make "yves" refused: read-only file system (the device failed a write)` --
so the log shows that writes stopped reaching the device; a volume mounted
read-only on purpose (Recovery, an unknown `ro_compat` bit) stays quiet.

The device has to say so for any of this to happen. The IDE driver used to
check only the bus master's status after a DMA write, and a disk that refused
the data reports that in its own status register (ERR or DF): the write was
reported done and the data was gone. Both are checked now, after each PIO
sector and after the write's cache flush too.

Why this is a filesystem decision and not the caller's: on the device that
motivated it (a USB stick whose flash wedged on the first write) each failed
write cost the host transport's full timeout, 30 s, and init alone makes eight
writes at boot before the desktop is spawned. Reads worked throughout. A
read-only root that reaches the desktop in seconds is strictly better than a
writable one that reaches nothing in minutes — and it says, in the log, why.

The one write that does **not** trip this is the root mirror's (§6.3): the
mirror is a safety net and its staging already proceeds without one on
failure; a failing mirror write is logged by that path and does not make the
volume read-only on its own.

## Validation status

- **§7 read-only on device failure** — `make test-ro-fallback`
  (tools/ro_fallback_test.py), two live-session boots of the same image: a
  control (no read-only line, root listed) and QEMU `blkdebug` failing every
  write with errno 5, reads untouched (`iotype = "write"`): the first commit
  fails, the read-only line appears, init's account directory is refused with
  `read-only file system` without reaching the device, 2 I/O errors in all,
  the root is listed and the desktop starts. Passed 2026-10-25, with
  `test-root-mirror`.
- **§6 root mirror** — `make test-root-mirror` (tools/root_mirror_test.py), three boots of the
  same image: healthy (mirror line absent, root listed), root block overwritten with noise
  (`bad node magic`, served from the mirror, root listed), and QEMU `blkdebug` injecting
  errno 5 on every read touching the root's sectors (`read (LBA 136) failed: I/O error`,
  served from the mirror, root listed). One primary failure logged per boot, then
  `using its mirror at block 32766 from now on`. `verify_embkfs.py` checks the mirror is
  byte-identical to the root. Passed 2026-10-01 alongside `test-crash`, `test-embkfs-crash`,
  `test-usb-all` and `test-laptop`.

Every claim above is covered by a live selftest in `kernel/selftests.c`:

```
test posix                  10 rename checks incl. atomic-replace
                            (destination holds the SOURCE's bytes, size 4 not 14)
test git repo               init/config/add/commit/log/status -> a real root commit
                            (the lockfile-rename protocol, end to end)
test ioperf                 the read path's counters -- run it BEFORE touching §4
test embkfs all             the v2.0-v2.2 regression sweep, unaffected by v2.3
test tcc link               libc.a resolving from the flat root (§5)
```

`verify_embkfs.py` remains the independent oracle: it re-reads what the kernel
wrote without sharing its code, which is how a reader bug is caught rather than
agreed with.
