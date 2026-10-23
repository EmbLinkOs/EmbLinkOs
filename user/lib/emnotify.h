#ifndef _EMNOTIFY_H_
#define _EMNOTIFY_H_

/* user/lib/emnotify.h -- tell the user something from outside your window.
 *
 * THE GAP THIS FILLS. Until now a program could only speak inside its own
 * window: a background task that finished, a save that failed, an app that
 * refused to start had nowhere to say so except a status line nobody is
 * looking at -- or the serial console, which on a real machine nobody can see
 * at all. The desktop's own "could not start that app" went to fd 1 and
 * vanished.
 *
 * FIRE AND FORGET. embk_notify returns as soon as the service has taken the
 * message, NOT when the banner goes away: a program that has just failed to
 * save your file must not then block for five seconds telling you so. The
 * service answers immediately and displays on its own time.
 *
 * IT IS ALSO ALLOWED TO FAIL. If no notification service is running the call
 * returns -EMBK_ENOENT and the caller carries on -- a message the user did not
 * see is not a reason to stop doing the work. Callers that can show the same
 * thing in their own window should still do that; a notification is for when
 * you CANNOT. */

#include "emsvc.h"

/* How loudly. The service decides what each means visually; this is the
 * caller saying what KIND of thing happened, not how it should look. */
enum { EMNOTE_INFO = 0, EMNOTE_WARN = 1, EMNOTE_FAIL = 2 };

struct emnote_req {
    uint32_t level;         /* EMNOTE_* */
    char     title[64];     /* one line -- who is speaking and about what */
    char     body[160];     /* the detail; may be empty */
};

struct emnote_rep {
    int32_t  taken;         /* 1 = queued for display */
};

static inline int embk_notify_level(int level, const char *title, const char *body) {
    struct emnote_req q;
    for (unsigned i = 0; i < sizeof q; i++) ((unsigned char *)&q)[i] = 0;
    q.level = (uint32_t)level;
    #define EMN_COPY(dst, src) do { if (src) { unsigned i = 0; \
        while (src[i] && i < sizeof(dst) - 1) { dst[i] = src[i]; i++; } dst[i] = 0; } } while (0)
    EMN_COPY(q.title, title);
    EMN_COPY(q.body,  body);
    #undef EMN_COPY

    struct emnote_rep r = { 0 };
    int n = emsvc_call("notify", EMSVC_T_NOTIFY, &q, sizeof q, &r, sizeof r);
    if (n < 0) return n;
    return r.taken ? 0 : -EMBK_EINVAL;
}

static inline int embk_notify(const char *title, const char *body) {
    return embk_notify_level(EMNOTE_INFO, title, body);
}

#endif /* _EMNOTIFY_H_ */
