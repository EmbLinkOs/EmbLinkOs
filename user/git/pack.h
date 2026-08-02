#ifndef EMBK_GIT_PACK_H
#define EMBK_GIT_PACK_H
/* Packfile unpacking (Documentation/technical/pack-format.txt): decode a v2
 * packfile into fully-resolved git objects -- inflate each entry, resolve ofs-
 * and ref-deltas against their bases, and name each object by its SHA-1. */
#include <stddef.h>
#include <stdint.h>

/* git object types (as in the loose-object header and pack entry). */
enum { OBJ_COMMIT = 1, OBJ_TREE = 2, OBJ_BLOB = 3, OBJ_TAG = 4 };

struct pack_obj {
    int      type;             /* OBJ_* (resolved: a delta takes its base's type) */
    uint8_t *data;             /* malloc'd content (caller frees) */
    size_t   size;
    uint8_t  sha[20];          /* SHA-1 of "<type> <size>\0" + content */
};

/* Unpack `pack` (len bytes, including the 12-byte header and 20-byte trailer)
 * into a malloc'd array of resolved objects. On success sets *out and returns
 * the object count; caller frees each (*out)[i].data and *out. Negative on a
 * malformed pack or an unresolvable delta. */
int pack_unpack(const uint8_t *pack, size_t len, struct pack_obj **out);

const char *pack_type_name(int type);   /* "commit"/"tree"/"blob"/"tag" */

#endif /* EMBK_GIT_PACK_H */
