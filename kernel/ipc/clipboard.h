/* kernel/ipc/clipboard.h -- the system clipboard.
 *
 * One machine-global text buffer with set/get semantics: the smallest thing
 * that makes copy/paste REAL across processes. It lives in the kernel because
 * the clipboard's whole job is to outlive the process that filled it -- an
 * app can exit right after copying and the paste must still work.
 *
 * Text only, by design (v1): a byte string, no MIME negotiation, no history.
 * Ownership questions (who may read the clipboard?) belong to the capability
 * model later; today every process in the session shares it, matching the
 * single-user desktop it serves. */
#ifndef __EMBK_CLIPBOARD_H__
#define __EMBK_CLIPBOARD_H__

#include <stdint.h>
#include "include/types.h"

#define CLIPBOARD_MAX (64 * 1024)

/* Replace the clipboard with `len` bytes from USER memory.
 * Returns 0, or -EMBK_EFAULT / -EMBK_EINVAL (too big). */
int64_t clipboard_set_user(const void *ubuf, size_t len);

/* Copy the clipboard into USER memory (up to `cap` bytes).
 * Returns the number of bytes the clipboard HOLDS (so a caller with a small
 * buffer learns the real size), or -EMBK_EFAULT. Empty clipboard returns 0. */
int64_t clipboard_get_user(void *ubuf, size_t cap);

#endif
