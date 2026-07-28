#ifndef KERNEL_FS_NAMESPACE_H
#define KERNEL_FS_NAMESPACE_H

#include <stdint.h>
#include "include/types.h"   /* bool, size_t, NULL -- NOT <stdbool.h> (kernel's own) */
#include "fs/vfs.h"          /* struct vnode -- the object handle a binding names */

/* Per-process NAMESPACE -- the "authority IS the namespace" model
 * (docs/USERSPACE_v2.md, UP2). Internally everything is an object handle
 * (a struct vnode = {mnt, ino, type}); a namespace is a small table mapping a
 * path PREFIX -> a root object handle (+ a read/write mode). Path resolution is
 * "namespace lookup, THEN object_open(root, relative)" -- the kernel never walks
 * from a global root; it always starts from a handle the process was HANDED.
 * A prefix nobody bound simply does not resolve (ENOENT = absence, not "denied").
 *
 * The two grants a process is born with -- this namespace and cap_set -- only
 * ever ATTENUATE parent -> child: you cannot hand down a name you do not hold,
 * nor widen a read-only binding to writable (see ns_attenuate). '..' can never
 * climb above a binding's root (the walk clamps at the root), so a binding is a
 * true containment boundary. */

#define NS_MAX_BINDINGS 8
#define NS_PREFIX_MAX   64      /* matches vfs_mount.at[64] */

enum ns_mode { NS_MODE_RW = 0, NS_MODE_RO = 1 };

struct ns_binding {
    char         prefix[NS_PREFIX_MAX]; /* absolute, e.g. "/", "/system" */
    struct vnode root;                  /* the root object handle */
    uint8_t      mode;                  /* enum ns_mode */
    bool         used;
};

struct namespace {
    struct ns_binding b[NS_MAX_BINDINGS];
    uint8_t           n;
    bool              active;   /* false => resolve against the global mount
                                 * table (kernel / pre-userspace compat) */
};

/* Add or replace a binding (a later bind of the same prefix wins). Longest
 * prefix wins at lookup, so bind order does not matter. Marks the namespace
 * active. Returns EMBK_OK or -EMBK_* (table full / bad prefix). */
int ns_bind(struct namespace *ns, const char *prefix,
            struct vnode root, uint8_t mode);

/* Longest-prefix match for absolute `path`. Fills root_out, the matched prefix
 * length (walk from path + *prefix_len_out), and the binding mode. Returns
 * EMBK_OK, or -EMBK_ENOENT when no binding names `path` -- ABSENCE, the caller
 * turns that into a plain "not found". Any out-pointer may be NULL. */
int ns_lookup(const struct namespace *ns, const char *path,
              struct vnode *root_out, size_t *prefix_len_out, uint8_t *mode_out);

/* Verbatim copy (full inheritance -- a child sees its parent's view until it is
 * narrowed at spawn). */
void ns_copy(struct namespace *dst, const struct namespace *src);

/* Build the "global view" from the live mount table: one RW binding per mount
 * point, plus a read-only binding for the sealed /system subtree
 * (docs/USERSPACE.md D2). This is the root-of-authority namespace init is born
 * with; children inherit it. If no filesystem is mounted yet (very early boot,
 * e.g. idle kthreads) it leaves the namespace INACTIVE, so those contexts fall
 * back to the global resolver and nothing breaks. */
void ns_seed_global(struct namespace *ns);

/* Attenuation: dst := requested, but every requested binding must be a SUBSET of
 * one the parent holds -- its prefix nameable under a parent binding, and its
 * mode no wider than that parent binding's (an RO parent binding can only grant
 * RO). Returns EMBK_OK, or -EMBK_EPERM if the request exceeds the parent. Used
 * by the spawn-grant path (UP2b) and proven by `test namespace`. */
int ns_attenuate(struct namespace *dst, const struct namespace *parent,
                 const struct namespace *requested);

/* Write-gate for the current process: EMBK_OK if `path` falls in a writable
 * binding (or the caller has no active namespace -- kernel/global context),
 * -EMBK_EROFS if it falls in a read-only binding. An unbound path returns
 * EMBK_OK here (its resolve will already fail with ENOENT). Call at every
 * path-based write entry point. */
int ns_check_writable(const char *path);

/* Boot-time selftest of the namespace engine (`test namespace`): lookup,
 * longest-prefix, absence, seeding, attenuation. Returns EMBK_OK or -EMBK_*. */
int ns_run_selftests(void);

/* Implemented in process.c: the current process's namespace, or NULL when
 * there is no running user process (early boot / kernel threads). Declared here
 * so vfs.c can consult it without pulling in all of process.h. */
struct namespace *process_current_ns(void);

#endif /* KERNEL_FS_NAMESPACE_H */
