#ifndef _EMSVC_H_
#define _EMSVC_H_

/* user/lib/emsvc.h -- how one program asks another for something.
 *
 * WHY THIS EXISTS. Until now this OS had exactly ONE service endpoint in the
 * whole system: /run/emlink.desktop, which the top bar "calls" by connecting
 * and immediately hanging up -- the connection itself is the message, because
 * there was no convention for sending one. Everything else an application
 * might want from the system it either did itself or did without: there is no
 * shared file picker, nothing can post a notification, nothing can ask what
 * windows exist.
 *
 * A service is a program listening at /run/emlink.<name>. A call is ONE
 * request message, ONE reply message, then the connection closes. That is the
 * whole protocol, and it is deliberately the smallest thing that works:
 *
 *   * one message each way means neither side needs a state machine, and a
 *     crashed service is indistinguishable from a refused connection (the
 *     endpoint vanishes with the process -- /run is RAM, kernel/ipc/endpoint.h);
 *   * the connection closing IS the end of the transaction, so nothing has to
 *     agree about framing beyond "one send, one recv";
 *   * a request carries a TYPE word first, so one endpoint can answer more
 *     than one question later without a second path to publish and find.
 *
 * ANCILLARY HANDLES ARE PART OF THE DESIGN, not an afterthought: embk_chan_send
 * carries them, and the reason it matters here is the file panel. In a
 * capability system the interesting version of "open a file" is that the
 * PICKER holds the authority and hands back only the chosen object -- an app
 * with no filesystem namespace could still open exactly the file the user
 * pointed at, and nothing else. The kernel cannot wrap an open file as a
 * handle yet (kernel/ipc/handle.h knows surfaces, channels, endpoints, pipes),
 * so today the panel returns a path; the reply struct already carries the slot
 * for the handle, so that change does not move the protocol. */

#include "embk.h"

#define EMSVC_PREFIX "/run/emlink."

/* Every request starts with one of these. The reply echoes it, so a client
 * that somehow reaches the wrong service notices rather than parsing garbage
 * into a struct that happens to be the same size. */
#define EMSVC_T_FILE_PANEL   0x01
#define EMSVC_T_NOTIFY       0x02

struct emsvc_head {
    uint32_t type;      /* EMSVC_T_* */
    uint32_t len;       /* payload bytes that follow this header */
};

/* Publish a service. `name` is the bare name ("files"), not the path.
 * Returns a listening ENDPOINT handle, or -EMBK_*. */
static inline int emsvc_listen(const char *name) {
    char path[64];
    int i = 0;
    for (const char *p = EMSVC_PREFIX; *p && i < (int)sizeof path - 1; p++) path[i++] = *p;
    for (const char *p = name;         *p && i < (int)sizeof path - 1; p++) path[i++] = *p;
    path[i] = 0;
    return embk_chan_listen(path);
}

/* Call a service: send `req` (type + payload), wait for the reply into `rep`.
 * Returns the reply's payload length, or -EMBK_* (including -EMBK_ENOENT when
 * nothing is listening -- an unavailable service is a normal, reportable
 * condition, not a crash).
 *
 * BLOCKS until the service answers, which for the file panel means "until the
 * user has chosen". A caller that cannot stop should not call this. */
static inline int emsvc_call(const char *name, uint32_t type,
                             const void *req, unsigned reqlen,
                             void *rep, unsigned repcap) {
    char path[64];
    int i = 0;
    for (const char *p = EMSVC_PREFIX; *p && i < (int)sizeof path - 1; p++) path[i++] = *p;
    for (const char *p = name;         *p && i < (int)sizeof path - 1; p++) path[i++] = *p;
    path[i] = 0;

    int ch = embk_chan_connect(path);
    if (ch < 0) return ch;

    /* Header and payload in ONE message: two sends would let a service see a
     * header with no body if the caller died between them. */
    unsigned char buf[512];
    if (sizeof(struct emsvc_head) + reqlen > sizeof buf) { embk_chan_close(ch); return -EMBK_E2BIG; }
    struct emsvc_head h = { type, reqlen };
    for (unsigned k = 0; k < sizeof h; k++) buf[k] = ((const unsigned char *)&h)[k];
    for (unsigned k = 0; k < reqlen; k++)   buf[sizeof h + k] = ((const unsigned char *)req)[k];

    int rc = embk_chan_send(ch, buf, (unsigned)(sizeof h + reqlen), 0, 0, 0);
    if (rc < 0) { embk_chan_close(ch); return rc; }

    unsigned got = 0;
    rc = embk_chan_recv(ch, buf, sizeof buf, &got, 0, 0);
    embk_chan_close(ch);
    if (rc < 0) return rc;
    if (got < sizeof(struct emsvc_head)) return -EMBK_EINVAL;

    struct emsvc_head rh;
    for (unsigned k = 0; k < sizeof rh; k++) ((unsigned char *)&rh)[k] = buf[k];
    if (rh.type != type) return -EMBK_EINVAL;      /* answered by the wrong thing */

    unsigned n = rh.len < repcap ? rh.len : repcap;
    for (unsigned k = 0; k < n; k++) ((unsigned char *)rep)[k] = buf[sizeof rh + k];
    return (int)n;
}

/* The service side of one call: take the next connection, read its request.
 * Returns the connected CHANNEL handle (the caller replies on it and closes
 * it), or -EMBK_*. `*out_type` and the payload are filled from the request. */
static inline int emsvc_accept(int listen_handle, uint32_t *out_type,
                               void *req, unsigned reqcap, unsigned *out_len) {
    int ch = embk_chan_accept(listen_handle);
    if (ch < 0) return ch;

    unsigned char buf[512];
    unsigned got = 0;
    int rc = embk_chan_recv(ch, buf, sizeof buf, &got, 0, 0);
    if (rc < 0 || got < sizeof(struct emsvc_head)) { embk_chan_close(ch); return rc < 0 ? rc : -EMBK_EINVAL; }

    struct emsvc_head h;
    for (unsigned k = 0; k < sizeof h; k++) ((unsigned char *)&h)[k] = buf[k];
    if (out_type) *out_type = h.type;

    unsigned n = h.len < reqcap ? h.len : reqcap;
    for (unsigned k = 0; k < n; k++) ((unsigned char *)req)[k] = buf[sizeof h + k];
    if (out_len) *out_len = n;
    return ch;
}

/* Reply and hang up. */
static inline int emsvc_reply(int ch, uint32_t type, const void *rep, unsigned replen) {
    unsigned char buf[512];
    if (sizeof(struct emsvc_head) + replen > sizeof buf) { embk_chan_close(ch); return -EMBK_E2BIG; }
    struct emsvc_head h = { type, replen };
    for (unsigned k = 0; k < sizeof h; k++) buf[k] = ((const unsigned char *)&h)[k];
    for (unsigned k = 0; k < replen; k++)   buf[sizeof h + k] = ((const unsigned char *)rep)[k];
    int rc = embk_chan_send(ch, buf, (unsigned)(sizeof h + replen), 0, 0, 0);
    embk_chan_close(ch);
    return rc;
}

#endif /* _EMSVC_H_ */
