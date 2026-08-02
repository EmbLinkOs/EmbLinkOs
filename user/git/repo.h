#ifndef EMBK_GIT_REPO_H
#define EMBK_GIT_REPO_H
/* Write a fetched+unpacked repository to disk: loose objects, refs, and a
 * checked-out working tree -- a real .git the on-OS git can operate on. */
#include <stddef.h>
#include <stdint.h>
#include "pack.h"

/* Create <dir> and <dir>/.git/{objects,refs/heads}. 0 or -1. */
int repo_init(const char *dir);

/* Write every object as a loose object under <dir>/.git/objects (zlib-deflated
 * "<type> <size>\0" + content, sha-addressed). Returns count written or -1. */
int repo_write_objects(const char *dir, const struct pack_obj *objs, int n);

/* Write HEAD -> refs/heads/<branch>, refs/heads/<branch> -> head_hex, config. */
int repo_write_refs(const char *dir, const char *branch, const char *head_hex);

/* Check out the working tree of the commit `head_hex` into <dir>. Returns the
 * number of files written, or -1. */
int repo_checkout(const char *dir, const struct pack_obj *objs, int n,
                  const char *head_hex);

#endif /* EMBK_GIT_REPO_H */
