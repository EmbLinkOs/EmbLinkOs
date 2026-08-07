/* user/web/cookie.h -- the cookie jar.
 *
 * A cookie is how a site remembers you between two requests, and without one a
 * browser cannot stay logged in to anything: every page load is a stranger
 * arriving. It is also the first piece of state this browser keeps that BELONGS
 * TO A SERVER rather than to the document -- which is why it lives here and not
 * in the DOM, and why it is scoped by host rather than by page.
 *
 * Bounded like everything a stranger controls: a fixed number of cookies, a
 * fixed size each. A site that sets more than that gets the ones that fit --
 * which is a working session rather than a refusal, and the same bargain the
 * form table and the image cache already make.
 *
 * Not persisted. The jar lives as long as the browser process, so a session
 * survives navigation but not a restart. That is a deliberate stopping point,
 * not an oversight: writing cookies to disk means deciding where, deciding who
 * else can read them, and deciding when they expire on a machine whose clock
 * may not have been set -- and this OS's answer to all three should be the
 * capability system, not a file whose path is hard-coded here.
 */
#ifndef _EMBLINK_WEB_COOKIE_H_
#define _EMBLINK_WEB_COOKIE_H_

#include <stddef.h>

/* Forget everything. */
void cookie_reset(void);

/* Take one `Set-Cookie:` value ("id=abc; Path=/; Max-Age=3600"), as sent by
 * `host`. Understood attributes: Path, Domain, Max-Age, Expires (only well
 * enough to spot a deletion), Secure, HttpOnly. Returns 0 if stored. */
int cookie_set(const char *host, const char *value);

/* Scan a whole response header block for Set-Cookie lines. Returns how many
 * were stored -- a server may send several, and they are separate headers
 * rather than one comma-joined list, which is the one place cookies differ
 * from every other HTTP header. */
int cookie_take_headers(const char *host, const char *headers);

/* The `Cookie:` value to send with a request for host+path, or 0 bytes if
 * there is nothing to send. Writes "a=1; b=2" without the header name. */
size_t cookie_header(const char *host, const char *path, int secure,
                     char *out, size_t cap);

/* document.cookie: the same as cookie_header but excluding HttpOnly cookies,
 * which a script is not allowed to see. That exclusion is the entire security
 * value of the flag, so it is enforced here rather than at the call site. */
size_t cookie_for_script(const char *host, const char *path,
                         char *out, size_t cap);

#endif /* _EMBLINK_WEB_COOKIE_H_ */
