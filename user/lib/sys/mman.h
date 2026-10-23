/* sys/mman.h -- EmbLink override header (newlib ships none).
 *
 * mmap IS REAL: a per-process list of mapped ranges (kernel/mm/vma.h),
 * DEMAND-PAGED -- a mapping costs nothing until a page is touched -- with
 * the permissions you ask for, at an address the kernel picks, and taken
 * back on munmap, page tables included.
 *
 *   MAP_ANONYMOUS|MAP_PRIVATE   zero-filled memory that can be PAGED OUT: it
 *                               lives in a kernel object that writes it to
 *                               the swap store under memory pressure and
 *                               reads it back on the next touch. Running out
 *                               of RAM slows a program down; it no longer
 *                               kills it. (`make test-swap` is the proof.)
 *   a FILE (fd >= 0)            real, over the unified page cache. MAP_SHARED
 *                               sees the file's bytes and changes them -- a
 *                               write is visible to read() at once and
 *                               reaches the device by writeback. MAP_PRIVATE
 *                               copies on the first write.
 *   mprotect                    real, and the other half of W^X: mmap will
 *                               not hand out a page that is writable AND
 *                               executable, so map W, emit, flip to X.
 *                               PROT_NONE is no access at all (a guard page),
 *                               not rounded to read-only.
 *
 * WHAT IS REFUSED, loudly rather than approximated:
 *
 *   MAP_SHARED|MAP_ANONYMOUS    ENOTSUP  -- shared with nobody: there is no
 *                                           fork, and the anonymous object
 *                                           has one mapper by design
 *                                           (kernel/mm/vm_object.h).
 *   MAP_FIXED / a non-NULL addr ENOTSUP  -- the kernel chooses the address.
 *   PROT_WRITE|PROT_EXEC        EINVAL   -- refused in the kernel; that page
 *                                           is the primitive every code
 *                                           injection needs.
 *   an unaligned file offset    EINVAL   -- there is no page it corresponds to.
 *   msync                       ENOSYS   -- writeback already covers
 *                                           durability; a caller asking by
 *                                           name is told no, not yes. */
#ifndef _EMBK_SYS_MMAN_H
#define _EMBK_SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _EMBK_SYS_MMAN_H */
