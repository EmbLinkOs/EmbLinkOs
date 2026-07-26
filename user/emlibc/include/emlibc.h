/* emlibc.h — the C library native to EmbLinkOS (docs/EMLIBC_Requirements.md).
 *
 * Non-POSIX by design: emlibc exposes what the OS actually does, in EmbLink's
 * own vocabulary, over the raw syscall ABI in <embk_syscall.h>. This umbrella
 * header carries the version and the shared "absent, not faked" contract; the
 * real surface lives in the standard headers beside it (string/stdlib/stdio/
 * errno/unistd), each built -nostdinc against the compiler's freestanding
 * headers only, so nothing here can accidentally reach newlib.
 *
 * THE RULE (docs/EMLIBC_Requirements.md §1): a function EmbLink has no
 * mechanism for is ABSENT here, not a lying ENOSYS stub. fork/exec, async
 * signal delivery, BSD sockets, mmap of arbitrary files, /proc: not provided.
 */
#ifndef _EMLIBC_H
#define _EMLIBC_H

#define EMLIBC_VERSION_MAJOR 0
#define EMLIBC_VERSION_MINOR 1
#define EMLIBC_VERSION_STR   "emlibc 0.1 (phase 1: rim + string + stdio)"

#endif /* _EMLIBC_H */
