#ifndef EMBK_GIT_PUSH_H
#define EMBK_GIT_PUSH_H
/* git push core (transport-independent): build the objects for a first commit,
 * serialize a git-receive-pack request (ref-update command + packfile), and
 * parse the report-status reply. The HTTPS + auth layer lives in gitpush.c. */
#include <stddef.h>
#include <stdint.h>
#include "pack.h"

/* Build a commit that adds/replaces `path` (which MAY contain '/', creating or
 * descending nested subtrees) with `content`.
 *   parent_hex == NULL: a FIRST commit -- fresh trees, no parent.
 *   parent_hex != NULL: an INCREMENTAL commit whose parent's objects (commit +
 *     the full tree + existing blobs) are in base[0..base_n), as fetched from
 *     the server. The trees ALONG the path are spliced (every other entry at
 *     each level preserved), so unrelated files/dirs survive; the commit gets
 *     `parent_hex` as parent.
 * Produces ONLY the new objects (the blob, one new tree per path level, and the
 * commit) -- unchanged blobs/subtrees already live on the server. Sets *out to a
 * malloc'd array of *nout objects (caller frees each .data and the array) and
 * writes the new commit's id to newcommit_hex. 0 or -1. */
int push_make_commit(const char *path, const void *content, size_t clen,
                     const char *msg, const char *author, uint32_t ts,
                     const char *parent_hex, const struct pack_obj *base, int base_n,
                     struct pack_obj **out, int *nout, char newcommit_hex[41]);

/* Serialize a git-receive-pack request: one pkt-line command
 * "<old_hex> <new_hex> <ref>\0report-status\n", a flush-pkt, then a packfile of
 * `objs`. old_hex is 40 zeros to CREATE a ref; a new_hex unrelated to old_hex is
 * a FORCE (non-fast-forward) update. Sets *req (malloc'd, caller frees) and
 * *reqlen. 0 or -1. */
int push_build_request(const char *ref, const char *old_hex, const char *new_hex,
                       const struct pack_obj *objs, int n,
                       uint8_t **req, size_t *reqlen);

/* Serialize a request that DELETES `ref` (new id = 40 zeros) with an empty
 * packfile. old_hex must be the ref's current id. Sets *req and *reqlen. 0/-1. */
int push_build_delete(const char *ref, const char *old_hex,
                      uint8_t **req, size_t *reqlen);

/* Parse a report-status reply: needs "unpack ok" AND "ok <ref>" (not "ng").
 * Returns 0 on an accepted push, -1 otherwise. */
int push_parse_status(const uint8_t *body, size_t len);

#endif /* EMBK_GIT_PUSH_H */
