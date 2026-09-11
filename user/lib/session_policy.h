#ifndef EMBK_SESSION_POLICY_H
#define EMBK_SESSION_POLICY_H

/* WHAT A USER'S SESSION MAY BE GIVEN -- the clamp init applies to every
 * binding in a session profile, whatever the profile says.
 *
 * A session IS its namespace (docs/USERSPACE_v2.md): what the desktop and
 * everything under it can name. init reads the profile with the full view and
 * grants what it lists, so the profile is the whole of a user's authority --
 * and the first version read it from /home/<user>/user.ns, a file IN THE
 * USER'S OWN WRITABLE HOME. Adding `rw /etc` there gave the next session the
 * account store; `rw /home` gave it everyone's files. Profiles now live in
 * /etc/sessions/<user>.ns, which no session can reach -- and this clamp holds
 * even if one is edited anyway:
 *
 *   /system             read-only, anyone
 *   /data/apps          read-only, anyone
 *   /home/<user>[/...]  the user's own home, read-write
 *   /run                read-write, anyone (endpoints live there)
 *   /data[/...]         read-write, ADMINISTRATORS only (installing software)
 *
 * Never /etc, never another user's home, never a path with a ".." or an empty
 * component -- a prefix is compared component by component, so "/home/al"
 * does not cover "/home/alice".
 *
 * Header-only and libc-free: init is freestanding, and the console test that
 * proves the clamp links newlib. One definition for both. */

/* Does `path` equal `root` or lie below it, component-wise? */
static inline int embk_sp_under(const char *path, const char *root) {
    int i = 0;
    while (root[i] && path[i] == root[i]) i++;
    if (root[i]) return 0;                        /* diverged inside root */
    return path[i] == 0 || path[i] == '/';
}

/* A clean absolute path: leading '/', no "//", no "." or ".." components, no
 * trailing '/' (except "/" itself, which is never granted anyway). */
static inline int embk_sp_clean(const char *p) {
    if (p[0] != '/') return 0;
    if (p[1] == 0) return 1;                                   /* "/" */
    for (int i = 0; p[i]; i++) {
        if (p[i] != '/') continue;
        char c1 = p[i + 1];
        if (c1 == '/' || c1 == 0) return 0;                    /* "//", or a trailing '/' */
        if (c1 == '.' && (p[i + 2] == '/' || p[i + 2] == 0)) return 0;                     /* "/." */
        if (c1 == '.' && p[i + 2] == '.' && (p[i + 3] == '/' || p[i + 3] == 0)) return 0;  /* "/.." */
    }
    return 1;
}

/* May a session for `user` (an administrator or not) be granted `prefix`,
 * read-only if `ro`? 1 or 0. */
static inline int embk_session_grant_ok(const char *user, int is_admin, const char *prefix, int ro) {
    if (!embk_sp_clean(prefix) || prefix[1] == 0) return 0;
    if (embk_sp_under(prefix, "/system") || embk_sp_under(prefix, "/data/apps"))
        return ro || (is_admin && embk_sp_under(prefix, "/data/apps"));
    if (embk_sp_under(prefix, "/run")) return 1;
    /* /home/<user> and below: build the root without libc. */
    char home[48];
    const char *h = "/home/";
    int n = 0;
    while (h[n]) { home[n] = h[n]; n++; }
    for (int k = 0; user[k] && n < (int)sizeof home - 1; k++) home[n++] = user[k];
    home[n] = 0;
    if (user[0] && embk_sp_under(prefix, home)) return 1;
    if (is_admin && embk_sp_under(prefix, "/data")) return 1;
    return 0;
}

#endif
