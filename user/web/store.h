/* user/web/store.h -- state that outlives the process.
 *
 * Two things need it: the cookie jar, so a session survives a restart, and
 * localStorage, which is the only place a page can keep anything of its own.
 * Both are key/value, both are scoped to an ORIGIN, and both are written by a
 * program that runs whatever the network hands it -- so they share one file
 * rather than two, and one answer to the question of where they may write.
 *
 * WHERE: /data/apps/vellum/state, and nowhere else. That is not a convention,
 * it is enforced -- vellum.ns now grants `rw /data/apps/vellum` and `ro
 * /system` and names nothing else, so the kernel refuses any path outside it.
 * The browser used to inherit the whole session namespace, so declaring this
 * makes it strictly MORE confined than it was before it could persist at all.
 *
 * WHO MAY READ IT: whoever can name that directory. On a multi-user desktop
 * that is the session's own tree, which is the same boundary that already
 * separates one user's documents from another's.
 *
 * WHEN THINGS EXPIRE: by the wall clock, and only when there is one. An
 * unset clock reads as "no opinion" everywhere in this browser rather than as
 * 1970 -- a machine whose RTC was never set must not silently throw away every
 * saved session on boot.
 *
 * Bounded like everything a stranger controls: a fixed number of keys per
 * origin, a fixed size each, and a fixed number of origins. A site that writes
 * more gets the ones that fit.
 */
#ifndef _EMBLINK_WEB_STORE_H_
#define _EMBLINK_WEB_STORE_H_

#include <stddef.h>

/* The file operations this module needs, injected -- so store.c depends on no
 * syscall layer and the whole thing is testable on a host. NULL = no
 * persistence, and everything still works for the life of the process. */
struct store_io {
    long (*read)(const char *path, char *buf, size_t cap);   /* -1 if absent */
    long (*write)(const char *path, const char *buf, size_t len);
};
void store_set_io(const struct store_io *io);
/* WHERE state lives. Set by the app, which is the only thing that can read
 * $HOME. Empty (the default) means no persistence at all. */
void store_set_dir(const char *dir);

/* Forget everything in memory (a new session, not a new page). */
void store_reset(void);

/* localStorage, scoped to `origin` (a host; "" for a local file, which gets
 * its own private area rather than sharing one with every other file). */
const char *store_get(const char *origin, const char *key);
int         store_set(const char *origin, const char *key, const char *value);
int         store_remove(const char *origin, const char *key);
int         store_clear(const char *origin);
/* The key at `index`, or NULL -- localStorage.key(i) and .length. */
const char *store_key_at(const char *origin, int index);
int         store_count(const char *origin);

/* Load from / save to the state directory. Saving is explicit rather than
 * automatic on every set: a page in a loop would otherwise write the disk flat.
 */
int store_load(void);
int store_save(void);

/* The cookie jar's persistence rides the same file, because it answers the
 * same three questions and a second file would have to answer them again. */
int store_put_blob(const char *name, const char *data, size_t len);
long store_get_blob(const char *name, char *out, size_t cap);

#endif /* _EMBLINK_WEB_STORE_H_ */
