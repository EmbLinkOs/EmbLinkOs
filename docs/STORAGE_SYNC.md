# File sync and short writeback

The page cache is an intermediate writeback stage. `vfs_fd_fsync` first drains
the file's cached dirty pages and then invokes the vnode filesystem's `sync`
operation. The second step is required for clean files and directories too:
replacing a filename has no dirty directory data pages, but still needs the
filesystem/device barrier. EMBKFS's callback checks the physical parent driver's
flush support and propagates failures. Filesystems without a sync callback keep
their existing behavior.

A backend `write` returning success with fewer bytes than requested leaves all
pages in that writeback run dirty and makes `vmo_flush` return EIO. Repeating the
run is safe because it writes at the same file offsets. Clearing the entire
run after a partial write would acknowledge bytes the backend never accepted.

`test document sync` uses a private mount and descriptor fixture. It forces a
short successful backend write, asserts dirty pages remain, retries the full
write, then checks that clean-file/directory sync calls reach the backend and
that an injected barrier error reaches the caller. `test pagecache` retains the
existing write-coalescing, fsync, shared-open coherence and eviction checks.
The native integration harness runs both and allows deferred IPC cleanup to
settle between tests that snapshot global object counts.

Atomic replacement uses EMBKFS v2.3 §1's single directory B-tree commit. These
checks establish software ordering and error propagation; physical power-loss
acceptance remains hardware-specific.
