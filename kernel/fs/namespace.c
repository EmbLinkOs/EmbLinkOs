/* kernel/fs/namespace.c -- per-process namespaces (docs/USERSPACE_v2.md UP2).
 *
 * A namespace maps a path PREFIX -> a root object handle (a vnode) + a mode.
 * The whole engine is deliberately tiny: a fixed table, longest-prefix lookup,
 * and a subset check for attenuation. It carries authority, not caching -- a
 * vnode owns nothing (see vfs.h), so copying a namespace is a plain struct copy.
 *
 * See ns_seed_global() for how init's "global view" is built from the live mount
 * table, and vfs.c (vfs_resolve) for how resolution consults the current
 * process's namespace before ever touching the global mount table. */

#include "fs/namespace.h"
#include "fs/vfs.h"
#include "include/errno.h"
#include "include/kstring.h"

/* Does binding `prefix` name `path`? Returns the matched prefix length (walk
 * the remainder from path + len), or 0 for no match. Mirrors vfs_mount_is_prefix
 * -- "/" names every absolute path; "/system" names "/system" and "/system/..".*/
static size_t ns_prefix_match(const char *path, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (plen == 0)
        return 0;
    if (prefix[0] != '/' || path[0] != '/')
        return 0;
    if (plen == 1)                       /* prefix == "/" : names everything */
        return 1;
    for (size_t i = 0; i < plen; i++)
        if (path[i] != prefix[i])
            return 0;
    if (path[plen] == '\0' || path[plen] == '/')
        return plen;
    return 0;                            /* "/sys" must not match "/system" */
}

int ns_bind(struct namespace *ns, const char *prefix,
            struct vnode root, uint8_t mode)
{
    if (!ns || !prefix || prefix[0] != '/')
        return -EMBK_EINVAL;
    if (strlen(prefix) >= NS_PREFIX_MAX)
        return -EMBK_ENAMETOOLONG;

    /* Replace an existing binding of the same prefix, else take a free slot. */
    struct ns_binding *slot = NULL;
    for (uint8_t i = 0; i < NS_MAX_BINDINGS; i++) {
        if (ns->b[i].used && strcmp(ns->b[i].prefix, prefix) == 0) { slot = &ns->b[i]; break; }
    }
    if (!slot) {
        for (uint8_t i = 0; i < NS_MAX_BINDINGS; i++)
            if (!ns->b[i].used) { slot = &ns->b[i]; if (i + 1 > ns->n) ns->n = i + 1; break; }
    }
    if (!slot)
        return -EMBK_ENOSPC;

    size_t k = 0;
    while (prefix[k] && k < NS_PREFIX_MAX - 1) { slot->prefix[k] = prefix[k]; k++; }
    slot->prefix[k] = '\0';
    slot->root = root;
    slot->mode = mode;
    slot->used = true;
    ns->active = true;
    return EMBK_OK;
}

int ns_lookup(const struct namespace *ns, const char *path,
              struct vnode *root_out, size_t *prefix_len_out, uint8_t *mode_out)
{
    if (!ns || !path || path[0] != '/')
        return -EMBK_EINVAL;

    const struct ns_binding *best = NULL;
    size_t best_len = 0;
    for (uint8_t i = 0; i < NS_MAX_BINDINGS; i++) {
        if (!ns->b[i].used)
            continue;
        size_t m = ns_prefix_match(path, ns->b[i].prefix);
        if (m && (!best || m > best_len)) { best = &ns->b[i]; best_len = m; }
    }
    if (!best)
        return -EMBK_ENOENT;             /* absence, not "denied" */

    if (root_out)       *root_out = best->root;
    if (prefix_len_out) *prefix_len_out = best_len;
    if (mode_out)       *mode_out = best->mode;
    return EMBK_OK;
}

void ns_copy(struct namespace *dst, const struct namespace *src)
{
    if (!dst || !src)
        return;
    memcpy(dst, src, sizeof(*dst));
}

void ns_seed_global(struct namespace *ns)
{
    if (!ns)
        return;
    memset(ns, 0, sizeof(*ns));          /* n = 0, active = false */

    /* One RW binding per live mount point ("/", "/run", ...). We resolve each
     * mount point through the GLOBAL resolver (this runs while the process being
     * seeded has no active namespace yet), which hands back the mount's root
     * object handle. */
    struct vfs_mount_info mi[NS_MAX_BINDINGS];
    int nm = vfs_mounts_snapshot(mi, NS_MAX_BINDINGS);
    for (int i = 0; i < nm; i++) {
        struct vnode root;
        if (vfs_resolve(mi[i].at, &root) == EMBK_OK)
            ns_bind(ns, mi[i].at, root, NS_MODE_RW);
    }

    if (!ns->active)                     /* nothing mounted yet -> stay global */
        return;

    /* Seal the OS: /system is read-only to every user process (USERSPACE.md D2).
     * A longer prefix than "/", so it wins for any path under /system. */
    struct vnode sysv;
    if (vfs_resolve("/system", &sysv) == EMBK_OK)
        ns_bind(ns, "/system", sysv, NS_MODE_RO);
}

int ns_attenuate(struct namespace *dst, const struct namespace *parent,
                 const struct namespace *requested)
{
    if (!dst || !parent || !requested)
        return -EMBK_EINVAL;

    /* Every requested binding must sit under a parent binding, with a mode no
     * wider than that parent binding's (RO parent can only re-grant RO). */
    for (uint8_t i = 0; i < NS_MAX_BINDINGS; i++) {
        if (!requested->b[i].used)
            continue;
        size_t bestlen = 0; uint8_t pmode = NS_MODE_RO; bool found = false;
        for (uint8_t j = 0; j < NS_MAX_BINDINGS; j++) {
            if (!parent->b[j].used)
                continue;
            size_t m = ns_prefix_match(requested->b[i].prefix, parent->b[j].prefix);
            if (m && (!found || m > bestlen)) { bestlen = m; pmode = parent->b[j].mode; found = true; }
        }
        if (!found)
            return -EMBK_EPERM;          /* names something the parent cannot */
        if (requested->b[i].mode == NS_MODE_RW && pmode == NS_MODE_RO)
            return -EMBK_EPERM;          /* cannot widen RO -> RW */
    }

    ns_copy(dst, requested);
    dst->active = true;
    return EMBK_OK;
}

int ns_check_writable(const char *path)
{
    struct namespace *ns = process_current_ns();
    if (!ns || !ns->active)
        return EMBK_OK;                  /* kernel / global context: unrestricted */

    uint8_t mode;
    if (ns_lookup(ns, path, NULL, NULL, &mode) != EMBK_OK)
        return EMBK_OK;                  /* unbound: the resolve will ENOENT */
    return (mode == NS_MODE_RO) ? -EMBK_EROFS : EMBK_OK;
}

/* ------------------------------------------------------------------ *
 * Boot-time selftest for the namespace ENGINE (`test namespace`). Pure
 * logic -- it builds namespaces in place and checks lookup, longest-prefix,
 * absence, seeding, and attenuation. The LIVE read-only enforcement (a ring-3
 * process refused a write to /system) is proven separately by init's boot
 * probe, since a kernel-context selftest has no active namespace of its own.
 * ------------------------------------------------------------------ */
#include "include/kprintf.h"

static struct vnode ns_fake_vnode(uint64_t ino)
{
    struct vnode v; v.mnt = NULL; v.ino = ino; v.type = 0;   /* opaque tag for the test */
    return v;
}

int ns_run_selftests(void)
{
    int failures = 0;
    #define NS_CHECK(cond, msg) do { \
        if (cond) { kprintf("  ns: PASS %s\n", msg); } \
        else      { kprintf("  ns: FAIL %s\n", msg); failures++; } \
    } while (0)

    kprintf("NS: selftest: begin\n");

    /* 1. Longest-prefix lookup + absence, on a hand-built narrowed namespace. */
    struct namespace ns; memset(&ns, 0, sizeof(ns));
    ns_bind(&ns, "/",       ns_fake_vnode(1), NS_MODE_RW);
    ns_bind(&ns, "/system", ns_fake_vnode(2), NS_MODE_RO);

    struct vnode root; size_t plen; uint8_t mode;
    NS_CHECK(ns_lookup(&ns, "/data/x", &root, &plen, &mode) == EMBK_OK &&
             root.ino == 1 && mode == NS_MODE_RW && plen == 1,
             "/data/x resolves via '/' (rw)");
    NS_CHECK(ns_lookup(&ns, "/system/bin/home.elf", &root, &plen, &mode) == EMBK_OK &&
             root.ino == 2 && mode == NS_MODE_RO && plen == 7,
             "/system/bin/home.elf resolves via '/system' (ro, longest prefix wins)");
    NS_CHECK(ns_lookup(&ns, "/system", &root, &plen, &mode) == EMBK_OK &&
             root.ino == 2 && plen == 7,
             "'/system' itself resolves to the /system root object");
    /* "/sys" must NOT match "/system" -- but it matches "/" (rw). */
    NS_CHECK(ns_lookup(&ns, "/sys", &root, &plen, &mode) == EMBK_OK &&
             root.ino == 1, "'/sys' does not falsely match '/system'");

    /* 2. Absence: a namespace bound to ONLY /system cannot name /data. */
    struct namespace only_sys; memset(&only_sys, 0, sizeof(only_sys));
    ns_bind(&only_sys, "/system", ns_fake_vnode(2), NS_MODE_RO);
    NS_CHECK(ns_lookup(&only_sys, "/data/x", NULL, NULL, NULL) == -EMBK_ENOENT,
             "/data is ABSENT (not 'denied') in a /system-only namespace");
    NS_CHECK(ns_lookup(&only_sys, "/system/y", NULL, NULL, NULL) == EMBK_OK,
             "/system/y is nameable in the /system-only namespace");

    /* 3. Attenuation: a child may narrow, never widen. */
    struct namespace child, out;
    /* 3a. Subset with a tightened mode (RW parent -> RO child): allowed. */
    memset(&child, 0, sizeof(child));
    ns_bind(&child, "/data/sub", ns_fake_vnode(9), NS_MODE_RO);
    NS_CHECK(ns_attenuate(&out, &ns, &child) == EMBK_OK,
             "child binding under a parent prefix, RW->RO, is allowed");
    /* 3b. Widen /system RO -> RW: refused. */
    memset(&child, 0, sizeof(child));
    ns_bind(&child, "/system/x", ns_fake_vnode(9), NS_MODE_RW);
    NS_CHECK(ns_attenuate(&out, &ns, &child) == -EMBK_EPERM,
             "widening a read-only parent binding to RW is refused");
    /* 3c. Name something the parent lacks: refused. */
    memset(&child, 0, sizeof(child));
    ns_bind(&child, "/system/x", ns_fake_vnode(9), NS_MODE_RO);
    NS_CHECK(ns_attenuate(&out, &only_sys, &child) == EMBK_OK,
             "a subset of a /system-only parent is allowed");
    memset(&child, 0, sizeof(child));
    ns_bind(&child, "/data/x", ns_fake_vnode(9), NS_MODE_RO);
    NS_CHECK(ns_attenuate(&out, &only_sys, &child) == -EMBK_EPERM,
             "naming /data under a /system-only parent is refused (unnameable)");

    /* 4. The live global seed: '/' present RW, '/system' present RO. */
    struct namespace g; ns_seed_global(&g);
    NS_CHECK(g.active, "ns_seed_global produced an active namespace");
    NS_CHECK(ns_lookup(&g, "/data/apps/x", NULL, NULL, &mode) == EMBK_OK && mode == NS_MODE_RW,
             "seeded: /data is writable");
    NS_CHECK(ns_lookup(&g, "/system/bin/home.elf", NULL, NULL, &mode) == EMBK_OK && mode == NS_MODE_RO,
             "seeded: /system is read-only");

    kprintf("NS: selftest: %s (%d failure%s)\n",
            failures == 0 ? "OK" : "FAIL", failures, failures == 1 ? "" : "s");
    #undef NS_CHECK
    return failures == 0 ? EMBK_OK : -EMBK_EINVAL;
}
