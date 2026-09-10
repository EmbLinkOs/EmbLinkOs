/* sys/mman.h -- EmbLink override header (newlib ships none).
 *
 * mmap IS REAL NOW, for the anonymous private case: the kernel keeps a per
 * process list of mapped ranges (kernel/mm/vma.h) and hands out pages with
 * the permissions you ask for, at an address it picks -- and takes them back
 * on munmap, page tables included. That is the one thing sbrk could never do:
 * the heap only ever grows.
 *
 * WHAT IS STILL REFUSED, and refused loudly rather than approximated:
 *
 *   mmap of a FILE (fd >= 0)   ENODEV   -- needs a page cache that does not
 *                                          exist yet. Handing back anonymous
 *                                          zeroes for a named file is a bug
 *                                          that surfaces far from the call.
 *   MAP_SHARED                 ENOTSUP  -- a MAP_SHARED that behaved as
 *                                          MAP_PRIVATE is two processes each
 *                                          believing they see the other's
 *                                          writes.
 *   MAP_FIXED / a non-NULL addr ENOTSUP -- the kernel chooses the address.
 *   PROT_WRITE|PROT_EXEC       EINVAL   -- refused in the kernel; that page
 *                                          is the primitive every code
 *                                          injection needs.
 *   msync                      ENOSYS   -- meaningless until MAP_SHARED file
 *                                          mappings exist.
 *
 * mprotect IS real, and is the other half of the W^X rule: mmap will not give
 * you a page that is writable and executable, and with mprotect you do not
 * need one -- map W, emit, flip to X. PROT_NONE is honoured as no access at
 * all (a guard page), not rounded to read-only.
 *
 * Mapped pages arrive ZEROED, and the whole range is allocated up front: a
 * large mapping costs its full size immediately. There is no demand paging. */
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
