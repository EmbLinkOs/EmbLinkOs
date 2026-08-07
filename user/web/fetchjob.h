/* user/web/fetchjob.h -- a fetch that does not freeze the window.
 *
 * net.c answers "what are the bytes for this location". This answers a
 * different question -- WHEN, and who waits for it -- and that is why it is its
 * own file. A GUI event loop cannot block: a TLS handshake to a real host is
 * several round trips, and every one of them is a frame the window does not
 * draw. The user sees a dead application.
 *
 * So the blocking fetch runs on a worker thread and the view polls. The whole
 * design rests on one property, checked rather than assumed: the fetch path is
 * ALLOCATION-FREE (net.c keeps its one TLS context static, and libtls itself
 * never allocates), so the worker and the UI thread never touch newlib's
 * allocator at the same time -- which matters because __malloc_lock is a stub
 * in this libc and malloc here is not thread-safe.
 *
 * ONE job at a time, on purpose. A browser navigates to one page at a time, and
 * a second concurrent fetch would need a second static TLS context and an
 * answer to "which one wins". Starting a fetch while one is in flight is
 * refused, and the caller shows that it is still loading.
 */
#ifndef _EMBLINK_WEB_FETCHJOB_H_
#define _EMBLINK_WEB_FETCHJOB_H_

#include <stddef.h>
#include "net.h"

/* Start fetching `url` into `buf`. The buffer belongs to the JOB until it
 * finishes -- do not touch it before fetchjob_poll reports done.
 * Returns 0, or -1 if a fetch is already in flight. */
int fetchjob_start(const char *url, char *buf, size_t cap);

/* 1 = finished (result written to *out, buffer is yours again),
 * 0 = still in flight, -1 = nothing running. */
int fetchjob_poll(struct vnet_result *out);

/* Is a fetch in flight right now? For the view's loading state. */
int fetchjob_busy(void);

/* Milliseconds since the current job started -- so a slow page can say so
 * rather than looking hung. 0 when idle. */
unsigned fetchjob_elapsed_ms(void);

/* What is being fetched, for the loading message. "" when idle. */
const char *fetchjob_url(void);

#endif /* _EMBLINK_WEB_FETCHJOB_H_ */
