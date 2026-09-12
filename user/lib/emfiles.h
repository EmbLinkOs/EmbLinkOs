#ifndef _EMFILES_H_
#define _EMFILES_H_

/* user/lib/emfiles.h -- ask the user for a file.
 *
 * The one piece of shared UI every operating system has and this one did not.
 * Before it, an application that wanted a file asked for a PATH TYPED INTO A
 * TEXT BOX -- which is what Note++ and the editor still did: a field labelled
 * "Path to open or save as". That is not a file picker, it is a prompt, and it
 * means every app either writes a browser of its own or makes the user
 * remember where things are.
 *
 * ONE CALL, and the panel is drawn by a separate program (the service at
 * /run/emlink.files), not by a library linked into the caller. That is the
 * part worth insisting on:
 *
 *   * every app's picker is the SAME picker -- the same places, the same
 *     sorting, the same keyboard behaviour -- and improving it improves all of
 *     them at once, without rebuilding a single application;
 *   * it is where this OS's capability model eventually pays off. The panel
 *     holds the authority to browse; the app gets back only what the user
 *     pointed at. Today that is a path (see emsvc.h for why not yet a handle),
 *     so an app still needs the namespace to open it -- but the SHAPE is
 *     already the one that lets a confined app open a file it could never have
 *     found on its own.
 *
 * embk_file_panel BLOCKS until the user chooses or cancels: it is a modal
 * question and pretending otherwise would mean every caller writing a state
 * machine for an answer it cannot proceed without. */

#include "emsvc.h"

enum { EMFILE_OPEN = 0, EMFILE_SAVE = 1 };

struct emfile_req {
    uint32_t mode;          /* EMFILE_OPEN or EMFILE_SAVE */
    char     title[48];     /* "" -> the panel picks one from the mode */
    char     start[192];    /* directory to open in; "" -> the user's home */
    char     suggest[96];   /* SAVE: the filename to start with */
};

struct emfile_rep {
    int32_t  chosen;        /* 1 = a file, 0 = cancelled */
    int32_t  handle;        /* RESERVED: the open object, once the kernel can
                             * pass one (emsvc.h). -1 today, always. */
    char     path[256];
};

/* Returns 1 (chosen, `out` filled), 0 (cancelled), or -EMBK_* -- including
 * -EMBK_ENOENT when no panel service is running, which a caller should report
 * rather than treat as a cancel: "you said no" and "nobody asked you" are
 * different answers. */
static inline int embk_file_panel(int mode, const char *title, const char *start,
                                  const char *suggest, char *out, int cap) {
    struct emfile_req q;
    for (unsigned i = 0; i < sizeof q; i++) ((unsigned char *)&q)[i] = 0;
    q.mode = (uint32_t)mode;

    #define EMF_COPY(dst, src) do { if (src) { unsigned i = 0; \
        while (src[i] && i < sizeof(dst) - 1) { dst[i] = src[i]; i++; } dst[i] = 0; } } while (0)
    EMF_COPY(q.title, title);
    EMF_COPY(q.start, start);
    EMF_COPY(q.suggest, suggest);
    #undef EMF_COPY

    struct emfile_rep r;
    for (unsigned i = 0; i < sizeof r; i++) ((unsigned char *)&r)[i] = 0;

    int n = emsvc_call("files", EMSVC_T_FILE_PANEL, &q, sizeof q, &r, sizeof r);
    if (n < 0) return n;
    if (!r.chosen) return 0;

    int i = 0;
    while (r.path[i] && i < cap - 1) { out[i] = r.path[i]; i++; }
    if (cap > 0) out[i] = 0;
    return 1;
}

#endif /* _EMFILES_H_ */
