#ifndef EMBLINK_WORKSPACE_H
#define EMBLINK_WORKSPACE_H
#include "document.h"
#define EMWS_SLOTS 8
struct emdoc_workspace {
    char directory[256], prefix[96];
    uint64_t hashes[EMWS_SLOTS];
    unsigned char present[EMWS_SLOTS];
    int ready;
};
/* Per-process drafts prevent two instances overwriting each other's work.
 * Recovery skips other live instances; files live in the user's home only. */
int emws_init(struct emdoc_workspace *, const char *app);
int emws_checkpoint(struct emdoc_workspace *, int slot, const struct emdoc_draft *, const char *text, int dirty);
/* Called at startup. `restore` returns the slot assigned, or -1 if full.
 * A recovered copy is synced before the abandoned file is removed. */
int emws_restore(struct emdoc_workspace *, const char *app, size_t capacity,
                 int (*restore)(const struct emdoc_draft *, const char *text));
int emws_forget(struct emdoc_workspace *, int slot);
#endif
