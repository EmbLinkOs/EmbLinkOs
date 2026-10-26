/* ==========================================================================
 * gs.h -- what gitstatus's parts share: a git repository read straight from
 * its files (objects loose and packed, refs, config, the index, ignore rules)
 * and the status gitstatusd reports of it (gs_status.c).
 *
 * Portable C over POSIX files and zlib: the daemon is EmbLinkOS's
 * /system/bin/gitstatus.elf, and the same sources build on the host, where
 * tools/gitstatus_golden.py holds them to gitstatusd itself.
 * ========================================================================== */
#ifndef EMBK_GITSTATUS_GS_H
#define EMBK_GITSTATUS_GS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define GS_PATH 1024

/* --- bytes ------------------------------------------------------------------ */

struct gs_buf { uint8_t *p; size_t n, cap; };
bool gs_buf_add(struct gs_buf *b, const void *data, size_t n);
bool gs_buf_str(struct gs_buf *b, const char *s);
void gs_buf_free(struct gs_buf *b);
/* A whole file; false if it cannot be read. NUL-terminated (not counted). */
bool gs_read_file(const char *path, struct gs_buf *out);
/* The first line of a small file, without its newline; false if missing. */
bool gs_read_line(const char *path, char *out, size_t cap);
bool gs_exists(const char *path);
bool gs_is_dir(const char *path);
void gs_join(char *out, size_t cap, const char *a, const char *b);

struct gs_oid { uint8_t b[20]; };
bool gs_oid_parse(const char *hex, struct gs_oid *out);       /* 40 hex digits */
void gs_oid_hex(const struct gs_oid *o, char out[41]);
bool gs_oid_eq(const struct gs_oid *a, const struct gs_oid *b);
bool gs_oid_zero(const struct gs_oid *o);
/* The blob name of `n` bytes: SHA-1 of "blob <n>\0" + data. */
void gs_hash_blob(const void *data, size_t n, struct gs_oid *out);

/* --- objects ------------------------------------------------------------------ */

enum { GS_COMMIT = 1, GS_TREE = 2, GS_BLOB = 3, GS_TAG = 4 };
struct gs_odb;
struct gs_odb *gs_odb_open(const char *objects_dir);
void gs_odb_close(struct gs_odb *odb);
/* An object's content and type; false if it is not there or cannot be read. */
bool gs_odb_read(struct gs_odb *odb, const struct gs_oid *id, int *type, struct gs_buf *out);

/* --- configuration -------------------------------------------------------------- */

/* Keys as git writes them back: section.subsection.key, section and key in
 * lower case. The last value of a key wins. */
struct gs_config { char **key, **value; size_t n, cap; };
void gs_config_read(struct gs_config *c, const char *path);
const char *gs_config_get(const struct gs_config *c, const char *key);
bool gs_config_bool(const struct gs_config *c, const char *key, bool def);
void gs_config_free(struct gs_config *c);

/* --- refs ------------------------------------------------------------------------ */

struct gs_repo;
/* `name` (HEAD, refs/heads/main) resolved: its object, and the branch it
 * names through symbolic refs ("refs/heads/main"), if any. */
bool gs_ref_resolve(struct gs_repo *r, const char *name, struct gs_oid *out, char *symref, size_t cap);
/* The last tag (in byte order) whose object, peeled, is `commit`. */
bool gs_tag_at(struct gs_repo *r, const struct gs_oid *commit, char *out, size_t cap);
int gs_stash_count(struct gs_repo *r);

/* --- the index ---------------------------------------------------------------- */

struct gs_entry {
    char *path;
    struct gs_oid id;
    uint32_t mode, size, mtime_s, mtime_ns, ino;
    int stage;
    bool assume_unchanged, skip_worktree, intent_to_add;
};
struct gs_index {
    struct gs_entry *e;
    size_t n;
    time_t mtime;              /* the index file's: entries as new as it are racy */
    long mtime_ns;
};
bool gs_index_read(const char *path, struct gs_index *out);
void gs_index_free(struct gs_index *ix);

/* --- ignore rules ------------------------------------------------------------- */

/* git's wildmatch: * ? [..] \x, and with `pathname` `/` only matched by `**`. */
bool gs_wildmatch(const char *pat, const char *text, bool pathname);

struct gs_rule { char *pat; bool negate, dir_only, anchored, has_slash; };
struct gs_rules { struct gs_rule *r; size_t n; char base[GS_PATH]; };   /* base: "" or "dir/" */
void gs_rules_load(struct gs_rules *rs, const char *file, const char *base);
void gs_rules_free(struct gs_rules *rs);
/* The verdict of one set for `path` (relative to the workdir): 1 ignored,
 * -1 re-included (!), 0 no rule speaks. */
int gs_rules_match(const struct gs_rules *rs, const char *path, bool is_dir);

/* --- the repository and its status --------------------------------------------- */

struct gs_options {
    long max_staged, max_unstaged, max_conflicted, max_untracked;   /* < 0: all */
    long dirty_max_index_size;                                      /* < 0: any */
    bool recurse_untracked_dirs;            /* -e */
    bool ignore_status_show_untracked;      /* -U */
    bool ignore_bash_show_untracked;        /* -W */
    bool ignore_bash_show_dirty_state;      /* -D */
    long max_summary;                       /* -z */
    bool filemode;                          /* trust the exec bit (false on EmbLinkOS: EMBKFS has none) */
};

/* What gitstatusd answers, field for field. */
struct gs_status {
    char workdir[GS_PATH], commit[41], branch[256], upstream[256], remote[256], remote_url[GS_PATH];
    char action[64];
    long index_size, staged, unstaged, conflicted, untracked, ahead, behind, stashes;
    char tag[256];
    long unstaged_deleted, staged_new, staged_deleted;
    char push_remote[256], push_remote_url[GS_PATH];
    long push_ahead, push_behind, skip_worktree, assume_unchanged;
    char encoding[64];
    char summary[1024];
};

struct gs_repo *gs_repo_open(const char *dir);       /* NULL: not in a repository */
void gs_repo_close(struct gs_repo *r);
const char *gs_repo_gitdir(const struct gs_repo *r);
/* `no_diff`: nothing that needs the index (counts zero). False on an error
 * reading the repository. */
bool gs_repo_status(struct gs_repo *r, const struct gs_options *o, bool no_diff, struct gs_status *out);

/* internals the parts share */
struct gs_repo {
    char gitdir[GS_PATH], commondir[GS_PATH], workdir[GS_PATH];
    struct gs_config cfg;
    struct gs_odb *odb;
    void *cache;                /* gs_status.c's: the HEAD tree, hashed files, ahead/behind */
};

#endif /* EMBK_GITSTATUS_GS_H */
