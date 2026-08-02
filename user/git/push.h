#ifndef EMBK_GIT_PUSH_H
#define EMBK_GIT_PUSH_H
/* git push core (transport-independent): build the objects for a first commit,
 * serialize a git-receive-pack request (ref-update command + packfile), and
 * parse the report-status reply. The HTTPS + auth layer lives in gitpush.c. */
#include <stddef.h>
#include <stdint.h>
#include "pack.h"

/* Build the three objects of a repository's FIRST commit -- a single file:
 *   out[0] blob   (content), out[1] tree (one 100644 entry), out[2] commit
 *   (no parent; tree + author/committer at epoch `ts`, +0000).
 * Fills each out[i].{type,data,size,sha} (caller frees .data) and returns the
 * new commit's 40-hex id in newcommit_hex. 0 or -1. */
int push_make_first_commit(const char *fname, const void *content, size_t clen,
                           const char *msg, const char *author, uint32_t ts,
                           struct pack_obj out[3], char newcommit_hex[41]);

/* Serialize a git-receive-pack request: one pkt-line command
 * "<old_hex> <new_hex> <ref>\0report-status\n", a flush-pkt, then a packfile of
 * `objs`. old_hex is 40 zeros to CREATE a ref. Sets *req (malloc'd, caller
 * frees) and *reqlen. 0 or -1. */
int push_build_request(const char *ref, const char *old_hex, const char *new_hex,
                       const struct pack_obj *objs, int n,
                       uint8_t **req, size_t *reqlen);

/* Parse a report-status reply: needs "unpack ok" AND "ok <ref>" (not "ng").
 * Returns 0 on an accepted push, -1 otherwise. */
int push_parse_status(const uint8_t *body, size_t len);

#endif /* EMBK_GIT_PUSH_H */
