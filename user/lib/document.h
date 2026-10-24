#ifndef EMBLINK_DOCUMENT_H
#define EMBLINK_DOCUMENT_H
#include <stddef.h>
#include <stdint.h>
/* Whole text documents. A stamp detects ordinary external edits before Save.
 * errno: EFBIG for oversize, EILSEQ for embedded NUL, EAGAIN for a conflict.
 * Reads never expose partial bytes; failed saves preserve the previous name.
 * Save writes an exclusive sibling, fsyncs it, renames, then fsyncs the parent.
 * A parent-sync failure can occur after replacement: callers retain dirty state.
 * The stamp check is advisory; concurrent writers need filesystem CAS support. */
struct emdoc_stamp { uint64_t hash, bytes; int exists; };
int emdoc_read(const char *path, char *text, size_t capacity, struct emdoc_stamp *stamp);
int emdoc_probe(const char *path, struct emdoc_stamp *stamp);
int emdoc_save(const char *path, const void *data, size_t length,
               const struct emdoc_stamp *expected, struct emdoc_stamp *saved);
uint64_t emdoc_hash(const void *bytes, size_t length);
/* Recoverable text snapshot. Versioned and checksummed, never mixed with the
 * user's original document. Paths and cursors are bounded on load. */
struct emdoc_draft {
    char path[256];
    uint32_t cursor;
    struct emdoc_stamp original;
};
int emdoc_draft_save(const char *file, const struct emdoc_draft *meta, const char *text);
int emdoc_draft_load(const char *file, struct emdoc_draft *meta, char *text, size_t capacity);
#endif
