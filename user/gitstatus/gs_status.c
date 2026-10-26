/* ==========================================================================
 * gs_status.c -- a repository found and described as gitstatusd describes
 * it: HEAD and its branch, upstream and push remote with the commits each
 * side has, the repository's state (merge, rebase n/m...), the HEAD tree
 * against the index (staged), the index against the working tree
 * (unstaged), conflicts, untracked files under the ignore rules, stashes,
 * the tag at HEAD, and the commit's summary.
 *
 * What does not change between prompts is kept per repository: the HEAD
 * tree by commit, a file's name by its stat data, the commits ahead and
 * behind by the two tips.
 * ========================================================================== */
#include "gs.h"
#include "sha1.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char EMPTY_BLOB[] = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

/* --- what is kept ----------------------------------------------------------- */

struct tentry { char *path; uint32_t mode; struct gs_oid id; };
struct memo { char *path; off_t size; time_t mtime; long mtime_ns; ino_t ino; struct gs_oid id; };
struct cache {
    bool have_tree;
    struct gs_oid tree_of;     /* the commit whose tree this is */
    struct tentry *tree;
    size_t ntree;
    struct memo *memo;         /* open addressing, by path */
    size_t nmemo, capmemo;
    struct gs_oid ab_a, ab_b;  /* the last ahead/behind asked, and its answer */
    long ab_ahead, ab_behind;
    bool have_ab;
    struct dlist **dirs;       /* directory listings by path, open addressing; each its own */
    size_t ndirs, capdirs;
};

/* A directory's entries, as read at `read_at`. Kept while its mtime holds
 * and it was read a full second after that mtime: an entry added in the same
 * second as the listing would not move the mtime the cache compares. */
struct dent { char *name; bool is_dir; };
struct dlist { char *dir; time_t mtime, read_at; long ns; struct dent *e; size_t n; };

static void dlist_clear(struct dlist *d) {
    for (size_t i = 0; i < d->n; i++) free(d->e[i].name);
    free(d->e);
    d->e = NULL;
    d->n = 0;
}

static void tree_free(struct cache *c) {
    for (size_t i = 0; i < c->ntree; i++) free(c->tree[i].path);
    free(c->tree);
    c->tree = NULL;
    c->ntree = 0;
    c->have_tree = false;
}

static uint32_t hash_str(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static struct memo *memo_slot(struct cache *c, const char *path, bool add) {
    if (add && (c->nmemo + 1) * 2 > c->capmemo) {      /* grow, rehash */
        size_t cap = c->capmemo ? c->capmemo * 2 : 256;
        struct memo *m = calloc(cap, sizeof *m);
        if (!m) return NULL;
        for (size_t i = 0; i < c->capmemo; i++) {
            if (!c->memo[i].path) continue;
            size_t j = hash_str(c->memo[i].path) & (cap - 1);
            while (m[j].path) j = (j + 1) & (cap - 1);
            m[j] = c->memo[i];
        }
        free(c->memo);
        c->memo = m;
        c->capmemo = cap;
    }
    if (!c->capmemo) return NULL;
    size_t j = hash_str(path) & (c->capmemo - 1);
    while (c->memo[j].path) {
        if (!strcmp(c->memo[j].path, path)) return &c->memo[j];
        j = (j + 1) & (c->capmemo - 1);
    }
    if (!add) return NULL;
    c->memo[j].path = strdup(path);
    if (!c->memo[j].path) return NULL;
    c->nmemo++;
    return &c->memo[j];
}

/* --- the repository ------------------------------------------------------------- */

static void parent_dir(char *p) {
    char *slash = strrchr(p, '/');
    if (!slash) { p[0] = 0; return; }
    if (slash == p) p[1] = 0;
    else *slash = 0;
}

static bool is_gitdir(const char *dir) {
    char p[GS_PATH + 16];
    snprintf(p, sizeof p, "%s/HEAD", dir);
    return gs_exists(p);
}

/* .git as a file: "gitdir: <path>", relative to where the file is. */
static bool gitfile(const char *file, const char *dir, char *out, size_t cap) {
    char line[GS_PATH];
    if (!gs_read_line(file, line, sizeof line) || strncmp(line, "gitdir: ", 8)) return false;
    if (line[8] == '/') snprintf(out, cap, "%s", line + 8);
    else snprintf(out, cap, "%s/%s", dir, line + 8);
    return true;
}

static void read_configs(struct gs_repo *r) {
    /* the person's, then the repository's: a later value wins */
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char path[GS_PATH];
    if (xdg && xdg[0]) { snprintf(path, sizeof path, "%s/git/config", xdg); gs_config_read(&r->cfg, path); }
    else if (home && home[0]) { snprintf(path, sizeof path, "%s/.config/git/config", home); gs_config_read(&r->cfg, path); }
    if (home && home[0]) { snprintf(path, sizeof path, "%s/.gitconfig", home); gs_config_read(&r->cfg, path); }
    gs_join(path, sizeof path, r->commondir, "config");
    gs_config_read(&r->cfg, path);
}

struct gs_repo *gs_repo_open(const char *dir) {
    char cur[GS_PATH], gitdir[GS_PATH];
    snprintf(cur, sizeof cur, "%s", dir);
    size_t n = strlen(cur);
    while (n > 1 && cur[n - 1] == '/') cur[--n] = 0;
    for (;;) {
        char dotgit[GS_PATH + 8];
        snprintf(dotgit, sizeof dotgit, "%s/.git", strcmp(cur, "/") ? cur : "");
        struct stat st;
        bool found = false;
        if (lstat(dotgit, &st) == 0) {
            if (S_ISDIR(st.st_mode)) { snprintf(gitdir, sizeof gitdir, "%s", dotgit); found = is_gitdir(gitdir); }
            else if (S_ISREG(st.st_mode)) found = gitfile(dotgit, strcmp(cur, "/") ? cur : "", gitdir, sizeof gitdir) && is_gitdir(gitdir);
        }
        if (found) break;
        if (!strcmp(cur, "/") || !cur[0]) return NULL;
        parent_dir(cur);
    }
    struct gs_repo *r = calloc(1, sizeof *r);
    struct cache *c = calloc(1, sizeof *c);
    if (!r || !c) { free(r); free(c); return NULL; }
    r->cache = c;
    snprintf(r->gitdir, sizeof r->gitdir, "%s", gitdir);
    snprintf(r->workdir, sizeof r->workdir, "%s", cur);
    char common[GS_PATH], cpath[GS_PATH + 16];
    snprintf(cpath, sizeof cpath, "%s/commondir", gitdir);
    if (gs_read_line(cpath, common, sizeof common) && common[0]) {
        if (common[0] == '/') snprintf(r->commondir, sizeof r->commondir, "%s", common);
        else snprintf(r->commondir, sizeof r->commondir, "%s/%s", gitdir, common);
    } else snprintf(r->commondir, sizeof r->commondir, "%s", gitdir);
    read_configs(r);
    char objects[GS_PATH];
    gs_join(objects, sizeof objects, r->commondir, "objects");
    r->odb = gs_odb_open(objects);
    if (!r->odb) { gs_repo_close(r); return NULL; }
    return r;
}

void gs_repo_close(struct gs_repo *r) {
    if (!r) return;
    struct cache *c = r->cache;
    if (c) {
        tree_free(c);
        for (size_t i = 0; i < c->capmemo; i++) free(c->memo[i].path);
        free(c->memo);
        for (size_t i = 0; i < c->capdirs; i++) if (c->dirs[i]) { dlist_clear(c->dirs[i]); free(c->dirs[i]->dir); free(c->dirs[i]); }
        free(c->dirs);
        free(c);
    }
    gs_odb_close(r->odb);
    gs_config_free(&r->cfg);
    free(r);
}

const char *gs_repo_gitdir(const struct gs_repo *r) { return r->gitdir; }

/* --- commits ------------------------------------------------------------------------ */

struct commit { struct gs_oid tree, parents[16]; int nparents; long long time; };

static bool commit_read(struct gs_repo *r, const struct gs_oid *id, struct commit *out, struct gs_buf *raw) {
    int type;
    struct gs_buf b = { 0 };
    struct gs_buf *buf = raw ? raw : &b;
    if (!gs_odb_read(r->odb, id, &type, buf) || type != GS_COMMIT) { gs_buf_free(&b); return false; }
    memset(out, 0, sizeof *out);
    for (char *line = (char *)buf->p; line && *line && *line != '\n';) {
        char *next = strchr(line, '\n');
        if (!strncmp(line, "tree ", 5)) gs_oid_parse(line + 5, &out->tree);
        else if (!strncmp(line, "parent ", 7) && out->nparents < 16) gs_oid_parse(line + 7, &out->parents[out->nparents++]);
        else if (!strncmp(line, "committer ", 10)) {
            char *gt = strrchr(line, '>');
            if (gt && (!next || gt < next)) out->time = strtoll(gt + 1, NULL, 10);
        }
        line = next ? next + 1 : NULL;
    }
    gs_buf_free(&b);
    return true;
}

/* --- ahead and behind ----------------------------------------------------------------
 * libgit2's git_graph_ahead_behind, which gitstatusd counts with: both tips
 * painted down the history (mark_parents) -- the newest commit next, a
 * commit both reach and its parents STALE -- for as long as anything queued
 * or any root commit is not stale; then a second walk from both tips counts
 * the commits only one side reached. */

enum { P1 = 1, P2 = 2, STALE = 4, RESULT = 8 };
struct node { struct gs_oid id; long long time; int flags, nparents; struct gs_oid parents[16]; bool parsed; };
struct walk {
    struct node *n;
    size_t count, cap;
    size_t *slot, nslot;       /* oid -> node + 1, open addressing */
    size_t *heap, nheap, capheap;
};

static size_t walk_node(struct walk *w, struct gs_repo *r, const struct gs_oid *id) {
    if ((w->count + 1) * 2 > w->nslot) {                /* grow the table */
        size_t ns = w->nslot ? w->nslot * 2 : 256;
        size_t *slot = calloc(ns, sizeof *slot);
        if (!slot) return (size_t)-1;
        for (size_t i = 0; i < w->count; i++) {
            size_t j = (w->n[i].id.b[0] | (size_t)w->n[i].id.b[1] << 8 | (size_t)w->n[i].id.b[2] << 16) & (ns - 1);
            while (slot[j]) j = (j + 1) & (ns - 1);
            slot[j] = i + 1;
        }
        free(w->slot);
        w->slot = slot;
        w->nslot = ns;
    }
    size_t j = (id->b[0] | (size_t)id->b[1] << 8 | (size_t)id->b[2] << 16) & (w->nslot - 1);
    while (w->slot[j]) {
        if (gs_oid_eq(&w->n[w->slot[j] - 1].id, id)) return w->slot[j] - 1;
        j = (j + 1) & (w->nslot - 1);
    }
    if (w->count == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 128;
        struct node *n = realloc(w->n, cap * sizeof *n);
        if (!n) return (size_t)-1;
        w->n = n;
        w->cap = cap;
    }
    struct node *n = &w->n[w->count];
    memset(n, 0, sizeof *n);
    n->id = *id;
    struct commit cm;
    if (commit_read(r, id, &cm, NULL)) {                /* missing (shallow): no parents */
        n->parsed = true;
        n->time = cm.time;
        n->nparents = cm.nparents;
        memcpy(n->parents, cm.parents, sizeof n->parents);
    }
    w->slot[j] = w->count + 1;
    return w->count++;
}

/* git_commit_list_time_cmp: the newest first */
static bool newer(const struct walk *w, size_t a, size_t b) { return w->n[a].time > w->n[b].time; }
static void heap_push(struct walk *w, size_t i) {
    if (w->nheap == w->capheap) {
        size_t cap = w->capheap ? w->capheap * 2 : 64;
        size_t *h = realloc(w->heap, cap * sizeof *h);
        if (!h) return;
        w->heap = h;
        w->capheap = cap;
    }
    size_t k = w->nheap++;
    w->heap[k] = i;
    while (k && newer(w, w->heap[k], w->heap[(k - 1) / 2])) {
        size_t t = w->heap[k]; w->heap[k] = w->heap[(k - 1) / 2]; w->heap[(k - 1) / 2] = t;
        k = (k - 1) / 2;
    }
}
static size_t heap_pop(struct walk *w) {
    size_t top = w->heap[0];
    w->heap[0] = w->heap[--w->nheap];
    for (size_t k = 0;;) {
        size_t l = 2 * k + 1, rr = l + 1, m = k;
        if (l < w->nheap && newer(w, w->heap[l], w->heap[m])) m = l;
        if (rr < w->nheap && newer(w, w->heap[rr], w->heap[m])) m = rr;
        if (m == k) break;
        size_t t = w->heap[k]; w->heap[k] = w->heap[m]; w->heap[m] = t;
        k = m;
    }
    return top;
}

static void ahead_behind(struct gs_repo *r, const struct gs_oid *a, const struct gs_oid *b, long *ahead, long *behind) {
    struct cache *c = r->cache;
    *ahead = *behind = 0;
    if (gs_oid_eq(a, b)) return;
    if (c->have_ab && gs_oid_eq(&c->ab_a, a) && gs_oid_eq(&c->ab_b, b)) { *ahead = c->ab_ahead; *behind = c->ab_behind; return; }
    struct walk w = { 0 };
    size_t *roots = NULL, nroots = 0;
    size_t one = walk_node(&w, r, a);
    if (one == (size_t)-1) goto done;
    w.n[one].flags |= P1;
    heap_push(&w, one);
    size_t two = walk_node(&w, r, b);
    if (two == (size_t)-1) goto done;
    w.n[two].flags |= P2;
    heap_push(&w, two);
    /* mark_parents */
    for (;;) {
        bool interesting = false;
        for (size_t k = 0; k < w.nheap && !interesting; k++) interesting = !(w.n[w.heap[k]].flags & STALE);
        for (size_t k = 0; k < nroots && !interesting; k++) interesting = !(w.n[roots[k]].flags & STALE);
        if (!interesting || !w.nheap) break;
        size_t i = heap_pop(&w);
        int flags = w.n[i].flags & (P1 | P2 | STALE);
        if (flags == (P1 | P2)) { w.n[i].flags |= RESULT; flags |= STALE; }
        for (int p = 0; p < w.n[i].nparents; p++) {
            struct gs_oid pid = w.n[i].parents[p];
            size_t j = walk_node(&w, r, &pid);
            if (j == (size_t)-1 || (w.n[j].flags & flags) == flags) continue;
            w.n[j].flags |= flags;
            heap_push(&w, j);
        }
        if (!w.n[i].nparents) {                         /* a root: the path down to it must be marked */
            size_t *grown = realloc(roots, (nroots + 1) * sizeof *roots);
            if (grown) { roots = grown; roots[nroots++] = i; }
        }
    }
    /* ahead_behind: count what one side reached alone */
    w.nheap = 0;
    one = walk_node(&w, r, a);
    two = walk_node(&w, r, b);
    heap_push(&w, one);
    heap_push(&w, two);
    while (w.nheap) {
        size_t i = heap_pop(&w);
        int f = w.n[i].flags;
        if ((f & RESULT) || (f & (P1 | P2)) == (P1 | P2)) continue;
        if (f & P1) (*ahead)++;
        else if (f & P2) (*behind)++;
        for (int p = 0; p < w.n[i].nparents; p++) {
            struct gs_oid pid = w.n[i].parents[p];
            size_t j = walk_node(&w, r, &pid);
            if (j != (size_t)-1) heap_push(&w, j);
        }
        w.n[i].flags |= RESULT;
    }
    c->ab_a = *a; c->ab_b = *b; c->ab_ahead = *ahead; c->ab_behind = *behind; c->have_ab = true;
done:
    free(roots);
    free(w.n);
    free(w.slot);
    free(w.heap);
}

/* --- the HEAD tree, flat ------------------------------------------------------------------ */

static int tentry_cmp(const void *a, const void *b) { return strcmp(((const struct tentry *)a)->path, ((const struct tentry *)b)->path); }

static bool flatten(struct gs_repo *r, const struct gs_oid *tree, const char *prefix, struct cache *c, size_t *cap, int depth) {
    int type;
    struct gs_buf b = { 0 };
    if (depth > 256 || !gs_odb_read(r->odb, tree, &type, &b) || type != GS_TREE) { gs_buf_free(&b); return false; }
    bool ok = true;
    for (size_t i = 0; i < b.n && ok;) {
        uint32_t mode = 0;
        while (i < b.n && b.p[i] != ' ') mode = mode * 8 + (uint32_t)(b.p[i++] - '0');
        const char *name = (const char *)b.p + i + 1;
        size_t nl = strlen(name);
        i += 1 + nl + 1;
        if (i + 20 > b.n) { ok = false; break; }
        struct gs_oid id;
        memcpy(id.b, b.p + i, 20);
        i += 20;
        size_t plen = strlen(prefix) + nl + 2;
        char *path = malloc(plen);
        if (!path) { ok = false; break; }
        snprintf(path, plen, "%s%s", prefix, name);
        if (mode == 040000) {
            strcat(path, "/");
            ok = flatten(r, &id, path, c, cap, depth + 1);
            free(path);
            continue;
        }
        if (c->ntree == *cap) {
            size_t ncap = *cap ? *cap * 2 : 256;
            struct tentry *t = realloc(c->tree, ncap * sizeof *t);
            if (!t) { free(path); ok = false; break; }
            c->tree = t;
            *cap = ncap;
        }
        c->tree[c->ntree++] = (struct tentry){ path, mode, id };
    }
    gs_buf_free(&b);
    return ok;
}

static bool head_tree(struct gs_repo *r, const struct gs_oid *commit) {
    struct cache *c = r->cache;
    if (c->have_tree && gs_oid_eq(&c->tree_of, commit)) return true;
    tree_free(c);
    struct commit cm;
    size_t cap = 0;
    if (!commit_read(r, commit, &cm, NULL) || !flatten(r, &cm.tree, "", c, &cap, 0)) { tree_free(c); return false; }
    qsort(c->tree, c->ntree, sizeof *c->tree, tentry_cmp);
    c->have_tree = true;
    c->tree_of = *commit;
    return true;
}

/* --- the working tree against the index -------------------------------------------------- */

static uint32_t normal_mode(mode_t m) {
    if (S_ISLNK(m)) return 0120000;
    if (S_ISDIR(m)) return 0160000;
    return (m & 0100) ? 0100755 : 0100644;
}

/* The blob name of a file in the working tree (a link: of its target). */
static bool worktree_oid(const char *path, const struct stat *st, struct gs_oid *out) {
    if (S_ISLNK(st->st_mode)) {
        char target[GS_PATH];
        ssize_t n = readlink(path, target, sizeof target);
        if (n < 0) return false;
        gs_hash_blob(target, (size_t)n, out);
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char head[32];
    int hl = snprintf(head, sizeof head, "blob %lld", (long long)st->st_size);
    struct sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, head, (size_t)hl + 1);
    char chunk[16384];
    size_t got, total = 0;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) { sha1_update(&c, chunk, got); total += got; }
    fclose(f);
    if ((off_t)total != st->st_size) return false;    /* it changed under us: modified */
    sha1_final(&c, out->b);
    return true;
}

static long stat_ns(const struct stat *st) {
#if defined(__APPLE__)
    return st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    return st->st_mtim.tv_nsec;
#else
    (void)st;
    return 0;
#endif
}

/* The submodule checked out at `path`: its HEAD. */
static bool submodule_head(const char *path, struct gs_oid *out) {
    struct gs_repo *sub = gs_repo_open(path);
    if (!sub) return false;
    bool ok = !strcmp(sub->workdir, path) && gs_ref_resolve(sub, "HEAD", out, NULL, 0);
    gs_repo_close(sub);
    return ok;
}

/* Is the entry different in the working tree? *deleted when it is gone. */
static bool entry_changed(struct gs_repo *r, const struct gs_index *ix, const struct gs_entry *e, bool filemode, bool *deleted) {
    struct cache *c = r->cache;
    char path[GS_PATH * 2];
    snprintf(path, sizeof path, "%s/%s", r->workdir, e->path);
    struct stat st;
    *deleted = false;
    if (lstat(path, &st)) { *deleted = true; return true; }
    uint32_t emode = e->mode & 0170000;
    if (emode == 0160000) {                             /* a submodule: its HEAD, not its contents */
        if (!S_ISDIR(st.st_mode)) return true;
        struct gs_oid head;
        return submodule_head(path, &head) && !gs_oid_eq(&head, &e->id);
    }
    if (emode == 0120000 ? !S_ISLNK(st.st_mode) : !S_ISREG(st.st_mode)) return true;     /* type changed */
    if (filemode && emode == 0100000 && normal_mode(st.st_mode) != e->mode) return true;
    bool size_same = (uint32_t)st.st_size == e->size;
    char hex[41];
    gs_oid_hex(&e->id, hex);
    bool smudged = e->size == 0 && strcmp(hex, EMPTY_BLOB);   /* git zeroes the size of racy entries */
    if (!size_same && !smudged) return true;
    long ns = stat_ns(&st);
    bool racy = (time_t)e->mtime_s > ix->mtime || ((time_t)e->mtime_s == ix->mtime && (long)e->mtime_ns >= ix->mtime_ns);
    if (size_same && !smudged && (uint32_t)st.st_mtime == e->mtime_s && (uint32_t)ns == e->mtime_ns && !racy) return false;
    /* the contents decide; the answer is kept for as long as the stat data holds */
    struct memo *m = memo_slot(c, e->path, false);
    if (m && m->size == st.st_size && m->mtime == st.st_mtime && m->mtime_ns == ns && m->ino == st.st_ino &&
        st.st_mtime < time(NULL) - 1)
        return !gs_oid_eq(&m->id, &e->id);
    struct gs_oid id;
    if (!worktree_oid(path, &st, &id)) return true;
    m = memo_slot(c, e->path, true);
    if (m) { m->size = st.st_size; m->mtime = st.st_mtime; m->mtime_ns = ns; m->ino = st.st_ino; m->id = id; }
    return !gs_oid_eq(&id, &e->id);
}

/* --- untracked files --------------------------------------------------------------------- */

static struct dlist *dlist_slot(struct cache *c, const char *dir) {
    if ((c->ndirs + 1) * 2 > c->capdirs) {
        size_t cap = c->capdirs ? c->capdirs * 2 : 64;
        struct dlist **d = calloc(cap, sizeof *d);
        if (!d) return NULL;
        for (size_t i = 0; i < c->capdirs; i++) {
            if (!c->dirs[i]) continue;
            size_t j = hash_str(c->dirs[i]->dir) & (cap - 1);
            while (d[j]) j = (j + 1) & (cap - 1);
            d[j] = c->dirs[i];
        }
        free(c->dirs);
        c->dirs = d;
        c->capdirs = cap;
    }
    size_t j = hash_str(dir) & (c->capdirs - 1);
    while (c->dirs[j]) {
        if (!strcmp(c->dirs[j]->dir, dir)) return c->dirs[j];
        j = (j + 1) & (c->capdirs - 1);
    }
    struct dlist *d = calloc(1, sizeof *d);
    if (!d || !(d->dir = strdup(dir))) { free(d); return NULL; }
    c->dirs[j] = d;
    c->ndirs++;
    return d;
}

/* The entries of `dir_rel` (relative, "" or "a/b/"): kept, or read. NULL if
 * it cannot be read. */
static const struct dlist *listing(struct gs_repo *r, const char *dir_rel) {
    struct cache *c = r->cache;
    char abs[GS_PATH * 2];
    snprintf(abs, sizeof abs, "%s/%s", r->workdir, dir_rel);
    struct stat st;
    if (stat(abs, &st)) return NULL;
    struct dlist *d = dlist_slot(c, dir_rel);
    if (!d) return NULL;
    if (d->read_at && d->mtime == st.st_mtime && d->ns == stat_ns(&st) && d->read_at > st.st_mtime + 1) return d;
    dlist_clear(d);
    DIR *dd = opendir(abs);
    if (!dd) return NULL;
    d->mtime = st.st_mtime;
    d->ns = stat_ns(&st);
    d->read_at = time(NULL);
    size_t cap = 0;
    for (struct dirent *e; (e = readdir(dd));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git")) continue;
        char full[GS_PATH * 3];
        snprintf(full, sizeof full, "%s/%s", abs, e->d_name);
        struct stat es;
        if (lstat(full, &es)) continue;
        if (d->n == cap) {
            cap = cap ? cap * 2 : 16;
            struct dent *g = realloc(d->e, cap * sizeof *g);
            if (!g) break;
            d->e = g;
        }
        d->e[d->n].name = strdup(e->d_name);
        if (!d->e[d->n].name) break;
        d->e[d->n++].is_dir = S_ISDIR(es.st_mode);
    }
    closedir(dd);
    return d;
}

struct walkctx {
    struct gs_repo *r;
    char **paths;              /* every index path, sorted */
    size_t npaths;
    struct gs_rules *sets;     /* per-directory .gitignore files, outermost first */
    size_t nsets, capsets;
    struct gs_rules exclude, global;
    bool recurse;
    long count, limit;
};

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The first index path >= `key`. */
static size_t lower_bound(const struct walkctx *w, const char *key) {
    size_t lo = 0, hi = w->npaths;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(w->paths[mid], key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static bool tracked_file(const struct walkctx *w, const char *rel) {
    size_t i = lower_bound(w, rel);
    return i < w->npaths && !strcmp(w->paths[i], rel);
}
static bool tracked_below(const struct walkctx *w, const char *dir_slash) {
    size_t i = lower_bound(w, dir_slash), n = strlen(dir_slash);
    return i < w->npaths && !strncmp(w->paths[i], dir_slash, n);
}

/* The deepest .gitignore that speaks decides; then info/exclude, then the person's. */
static bool ignored(const struct walkctx *w, const char *rel, bool is_dir) {
    for (size_t i = w->nsets; i-- > 0;) {
        int v = gs_rules_match(&w->sets[i], rel, is_dir);
        if (v) return v > 0;
    }
    int v = gs_rules_match(&w->exclude, rel, is_dir);
    if (v) return v > 0;
    return gs_rules_match(&w->global, rel, is_dir) > 0;
}

static void push_rules(struct walkctx *w, const char *dir_rel) {
    char file[GS_PATH * 2];
    snprintf(file, sizeof file, "%s/%s.gitignore", w->r->workdir, dir_rel);
    if (w->nsets == w->capsets) {
        size_t cap = w->capsets ? w->capsets * 2 : 16;
        struct gs_rules *s = realloc(w->sets, cap * sizeof *s);
        if (!s) return;
        w->sets = s;
        w->capsets = cap;
    }
    gs_rules_load(&w->sets[w->nsets++], file, dir_rel);
}
static void pop_rules(struct walkctx *w) { if (w->nsets) gs_rules_free(&w->sets[--w->nsets]); }

static bool nested_repo(const char *abs) {
    char p[GS_PATH * 2];
    snprintf(p, sizeof p, "%s/.git", abs);
    return gs_exists(p);
}

/* Untracked files below an untracked directory (`all`: count them all; else
 * stop at the first). */
static long untracked_in(struct walkctx *w, const char *dir_rel, bool all) {
    const struct dlist *d = listing(w->r, dir_rel);
    if (!d) return 0;
    push_rules(w, dir_rel);
    long n = 0;
    for (size_t i = 0; i < d->n && (all || !n); i++) {
        char rel[GS_PATH * 2], full[GS_PATH * 3];
        snprintf(rel, sizeof rel, "%s%s", dir_rel, d->e[i].name);
        if (ignored(w, rel, d->e[i].is_dir)) continue;
        if (d->e[i].is_dir) {
            snprintf(full, sizeof full, "%s/%s", w->r->workdir, rel);
            if (nested_repo(full)) { n++; continue; }
            char sub[GS_PATH * 2 + 2];
            snprintf(sub, sizeof sub, "%s/", rel);
            n += untracked_in(w, sub, all);
        } else n++;
    }
    pop_rules(w);
    return n;
}

static void walk_dir(struct walkctx *w, const char *dir_rel, bool in_ignored) {
    if (w->limit >= 0 && w->count >= w->limit) return;
    const struct dlist *d = listing(w->r, dir_rel);
    if (!d) return;
    push_rules(w, dir_rel);
    for (size_t i = 0; i < d->n; i++) {
        if (w->limit >= 0 && w->count >= w->limit) break;
        char rel[GS_PATH * 2], full[GS_PATH * 3];
        snprintf(rel, sizeof rel, "%s%s", dir_rel, d->e[i].name);
        if (tracked_file(w, rel)) continue;
        if (d->e[i].is_dir) {
            char sub[GS_PATH * 2 + 2];
            snprintf(sub, sizeof sub, "%s/", rel);
            if (tracked_below(w, sub)) {                /* partly tracked: go in */
                walk_dir(w, sub, in_ignored || ignored(w, rel, true));
            } else if (!in_ignored && !ignored(w, rel, true)) {
                snprintf(full, sizeof full, "%s/%s", w->r->workdir, rel);
                if (nested_repo(full)) w->count++;      /* a repository of its own: one */
                else {
                    long n = untracked_in(w, sub, w->recurse);
                    w->count += w->recurse ? n : n > 0;
                }
            }
        } else if (!in_ignored && !ignored(w, rel, false)) w->count++;
    }
    pop_rules(w);
}

static long count_untracked(struct gs_repo *r, const struct gs_index *ix, bool recurse, long limit) {
    struct walkctx w = { 0 };
    w.r = r;
    w.recurse = recurse;
    w.limit = limit;
    w.paths = malloc((ix->n ? ix->n : 1) * sizeof *w.paths);
    if (!w.paths) return 0;
    for (size_t i = 0; i < ix->n; i++) w.paths[w.npaths++] = ix->e[i].path;
    qsort(w.paths, w.npaths, sizeof *w.paths, path_cmp);
    char path[GS_PATH * 2];
    gs_join(path, sizeof path, r->commondir, "info/exclude");
    gs_rules_load(&w.exclude, path, "");
    const char *global = gs_config_get(&r->cfg, "core.excludesfile");
    const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    if (global && global[0] == '~' && home) snprintf(path, sizeof path, "%s%s", home, global + 1);
    else if (global) snprintf(path, sizeof path, "%s", global);
    else if (xdg && xdg[0]) snprintf(path, sizeof path, "%s/git/ignore", xdg);
    else if (home) snprintf(path, sizeof path, "%s/.config/git/ignore", home);
    else path[0] = 0;
    gs_rules_load(&w.global, path, "");
    walk_dir(&w, "", false);
    while (w.nsets) pop_rules(&w);
    free(w.sets);
    gs_rules_free(&w.exclude);
    gs_rules_free(&w.global);
    free(w.paths);
    return w.count;
}

/* --- the repository's state: merge, rebase n/m... ----------------------------------------- */

static void action(struct gs_repo *r, char *out, size_t cap) {
    char p[GS_PATH + 64], a[32], b[32];
    out[0] = 0;
    #define AT(name) (snprintf(p, sizeof p, "%s/%s", r->gitdir, name), gs_exists(p))
    #define LINE(name, buf) (snprintf(p, sizeof p, "%s/%s", r->gitdir, name), gs_read_line(p, buf, sizeof buf))
    if (AT("rebase-merge")) {
        const char *kind = AT("rebase-merge/interactive") ? "rebase-i" : "rebase-m";
        if (LINE("rebase-merge/msgnum", a) && LINE("rebase-merge/end", b)) snprintf(out, cap, "%s %s/%s", kind, a, b);
        else snprintf(out, cap, "%s", kind);
    } else if (AT("rebase-apply")) {
        const char *kind = AT("rebase-apply/rebasing") ? "rebase" : AT("rebase-apply/applying") ? "am" : "am/rebase";
        if (LINE("rebase-apply/next", a) && LINE("rebase-apply/last", b)) snprintf(out, cap, "%s %s/%s", kind, a, b);
        else snprintf(out, cap, "%s", kind);
    } else if (AT("MERGE_HEAD")) snprintf(out, cap, "merge");
    else if (AT("REVERT_HEAD")) snprintf(out, cap, AT("sequencer/todo") ? "revert-seq" : "revert");
    else if (AT("CHERRY_PICK_HEAD")) snprintf(out, cap, AT("sequencer/todo") ? "cherry-seq" : "cherry");
    else if (AT("BISECT_LOG")) snprintf(out, cap, "bisect");
    #undef AT
    #undef LINE
}

/* libgit2's git_commit_summary: the first paragraph, its line breaks one space. */
static void summary_of(const char *msg, char *out, size_t cap, long max) {
    struct gs_buf s = { 0 };
    while (*msg == '\n') msg++;
    const char *space = NULL;
    bool newline = false;
    for (; *msg; msg++) {
        char ch = *msg;
        if (ch == '\n' && (!msg[1] || msg[1] == '\n')) break;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') {
            if (!space) { space = msg; newline = false; }
            newline |= ch == '\n';
            continue;
        }
        if (space) {
            if (newline) gs_buf_add(&s, " ", 1);
            else gs_buf_add(&s, space, (size_t)(msg - space));
            space = NULL;
        }
        gs_buf_add(&s, &ch, 1);
    }
    size_t n = s.n;
    if (max >= 0 && n > (size_t)max) n = (size_t)max;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, s.p, n);
    out[n] = 0;
    gs_buf_free(&s);
}

/* --- the answer ------------------------------------------------------------------------------ */

static long cap_at(long n, long max) { return max >= 0 && n > max ? max : n; }

/* Whether the index holds `path` in conflict (stages 1-3); it is sorted by path. */
static bool in_conflict(const struct gs_index *ix, const char *path) {
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(ix->e[mid].path, path) < 0) lo = mid + 1; else hi = mid;
    }
    for (; lo < ix->n && !strcmp(ix->e[lo].path, path); lo++) if (ix->e[lo].stage) return true;
    return false;
}

static bool tracking(struct gs_repo *r, const char *remote, const char *branch, struct gs_oid *out) {
    char ref[GS_PATH];
    if (!strcmp(remote, ".")) snprintf(ref, sizeof ref, "refs/heads/%s", branch);
    else snprintf(ref, sizeof ref, "refs/remotes/%s/%s", remote, branch);
    return gs_ref_resolve(r, ref, out, NULL, 0);
}

bool gs_repo_status(struct gs_repo *r, const struct gs_options *o, bool no_diff, struct gs_status *s) {
    memset(s, 0, sizeof *s);
    struct cache *c = r->cache;
    snprintf(s->workdir, sizeof s->workdir, "%s", r->workdir);
    char sym[GS_PATH];
    struct gs_oid head;
    bool born = gs_ref_resolve(r, "HEAD", &head, sym, sizeof sym);
    if (born) gs_oid_hex(&head, s->commit);
    if (!strncmp(sym, "refs/heads/", 11)) snprintf(s->branch, sizeof s->branch, "%s", sym + 11);
    /* upstream and push remote */
    if (s->branch[0]) {
        char key[512];
        snprintf(key, sizeof key, "branch.%s.remote", s->branch);
        const char *remote = gs_config_get(&r->cfg, key);
        snprintf(key, sizeof key, "branch.%s.merge", s->branch);
        const char *merge = gs_config_get(&r->cfg, key);
        struct gs_oid up;
        if (remote && merge) {
            const char *name = !strncmp(merge, "refs/heads/", 11) ? merge + 11 : merge;
            if (tracking(r, remote, name, &up)) {
                snprintf(s->upstream, sizeof s->upstream, "%s", name);
                snprintf(s->remote, sizeof s->remote, "%s", remote);
                snprintf(key, sizeof key, "remote.%s.url", remote);
                const char *url = gs_config_get(&r->cfg, key);
                snprintf(s->remote_url, sizeof s->remote_url, "%s", url ? url : "");
                if (born) ahead_behind(r, &head, &up, &s->ahead, &s->behind);
            }
        }
        snprintf(key, sizeof key, "branch.%s.pushremote", s->branch);
        const char *push = gs_config_get(&r->cfg, key);
        if (!push) push = gs_config_get(&r->cfg, "remote.pushdefault");
        struct gs_oid pt;
        if (push && tracking(r, push, s->branch, &pt)) {
            snprintf(s->push_remote, sizeof s->push_remote, "%s", push);
            snprintf(key, sizeof key, "remote.%s.url", push);
            const char *url = gs_config_get(&r->cfg, key);
            snprintf(s->push_remote_url, sizeof s->push_remote_url, "%s", url ? url : "");
            if (born) ahead_behind(r, &head, &pt, &s->push_ahead, &s->push_behind);
        }
    }
    action(r, s->action, sizeof s->action);
    s->stashes = gs_stash_count(r);
    if (born) {
        gs_tag_at(r, &head, s->tag, sizeof s->tag);
        struct commit cm;
        struct gs_buf raw = { 0 };
        if (commit_read(r, &head, &cm, &raw)) {
            const char *msg = strstr((const char *)raw.p, "\n\n");
            for (char *line = (char *)raw.p; line && (!msg || line < msg);) {
                char *next = strchr(line, '\n');
                if (!strncmp(line, "encoding ", 9)) {
                    size_t n = next ? (size_t)(next - line - 9) : strlen(line + 9);
                    snprintf(s->encoding, sizeof s->encoding, "%.*s", (int)n, line + 9);
                }
                line = next ? next + 1 : NULL;
            }
            summary_of(msg ? msg + 2 : "", s->summary, sizeof s->summary, o->max_summary);
        }
        gs_buf_free(&raw);
    }
    if (no_diff) return true;

    /* the index, and what is configured to be shown */
    char path[GS_PATH];
    gs_join(path, sizeof path, r->gitdir, "index");
    struct gs_index ix;
    if (!gs_index_read(path, &ix)) return false;
    s->index_size = (long)ix.n;
    bool dirty = o->ignore_bash_show_dirty_state || gs_config_bool(&r->cfg, "bash.showdirtystate", true);
    const char *su = gs_config_get(&r->cfg, "status.showuntrackedfiles");
    bool untracked = (o->ignore_status_show_untracked || !su || strcasecmp(su, "no")) &&
                     (o->ignore_bash_show_untracked || gs_config_bool(&r->cfg, "bash.showuntrackedfiles", true));
    bool big = o->dirty_max_index_size >= 0 && s->index_size > o->dirty_max_index_size;
    bool filemode = o->filemode && gs_config_bool(&r->cfg, "core.filemode", true);
    if (dirty) {
        /* conflicts: the paths with stages */
        for (size_t i = 0; i < ix.n; i++)
            if (ix.e[i].stage && (i == 0 || !ix.e[i - 1].stage || strcmp(ix.e[i - 1].path, ix.e[i].path))) s->conflicted++;
        /* staged: the HEAD tree against stage 0 */
        struct tentry *t = NULL;
        size_t nt = 0;
        if (born && head_tree(r, &head)) { t = c->tree; nt = c->ntree; }
        else if (born) { gs_index_free(&ix); return false; }
        struct gs_entry **idx = malloc((ix.n ? ix.n : 1) * sizeof *idx);
        size_t ni = 0;
        if (!idx) { gs_index_free(&ix); return false; }
        for (size_t i = 0; i < ix.n; i++) if (!ix.e[i].stage) idx[ni++] = &ix.e[i];
        /* paths in conflict count there, not here */
        size_t a = 0, b = 0;
        while (a < nt || b < ni) {
            int cmp = a >= nt ? 1 : b >= ni ? -1 : strcmp(t[a].path, idx[b]->path);
            if (cmp < 0) {
                if (!in_conflict(&ix, t[a].path)) { s->staged++; s->staged_deleted++; }
                a++;
            } else if (cmp > 0) {
                if (!idx[b]->intent_to_add) { s->staged++; s->staged_new++; }
                b++;
            } else {
                if (!gs_oid_eq(&t[a].id, &idx[b]->id) || t[a].mode != idx[b]->mode) s->staged++;
                a++; b++;
            }
        }
        /* unstaged: stage 0 against the working tree */
        if (!big) {
            for (size_t i = 0; i < ni; i++) {
                const struct gs_entry *e = idx[i];
                if (e->skip_worktree) { s->skip_worktree++; continue; }
                if (e->assume_unchanged) { s->assume_unchanged++; continue; }
                bool deleted;
                if (entry_changed(r, &ix, e, filemode, &deleted)) {
                    s->unstaged++;
                    if (deleted) s->unstaged_deleted++;
                }
            }
        } else
            for (size_t i = 0; i < ni; i++) { s->skip_worktree += idx[i]->skip_worktree; s->assume_unchanged += !idx[i]->skip_worktree && idx[i]->assume_unchanged; }
        free(idx);
    }
    if (untracked && !big) s->untracked = count_untracked(r, &ix, o->recurse_untracked_dirs, o->max_untracked);
    gs_index_free(&ix);
    s->staged = cap_at(s->staged, o->max_staged);
    s->unstaged = cap_at(s->unstaged, o->max_unstaged);
    s->conflicted = cap_at(s->conflicted, o->max_conflicted);
    s->untracked = cap_at(s->untracked, o->max_untracked);
    return true;
}
