/* user/system/notifyd/notifyd.c -- banners, so a program can speak from
 * outside its own window.
 *
 * Until this existed, anything a program had to say when it was not the thing
 * you were looking at had nowhere to go. The desktop's own "could not start
 * that app" went to fd 1 -- the serial console, which on the machine this OS
 * is meant to run on is a cable nobody has plugged in.
 *
 * TWO THREADS, AND THE REASON IS THE SHAPE OF THE PROBLEM. A service has to
 * wait for callers; a window has to be drawn continuously. embk_chan_accept
 * blocks, so one thread cannot do both. So: an accept thread takes messages
 * and answers them IMMEDIATELY (the caller is not made to wait for a banner it
 * will not read), and the main thread owns the window.
 *
 * THE WINDOW ONLY EXISTS WHILE THERE IS SOMETHING TO SAY. That is not
 * tidiness: a translucent window parked in the corner would swallow every
 * click that landed on it, so an empty notifier would quietly eat a rectangle
 * of your desktop forever. With nothing to show there is no window at all, and
 * the corner belongs to whatever is underneath. */

#include <stdio.h>
#include <string.h>

#include "embk.h"
#include "emsvc.h"
#include "emnotify.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define NOTE_MAX     4          /* banners on screen at once */
#define NOTE_MS   6000          /* how long one stays up */
#define PANEL_W    340
#define ROW_H       74

struct note {
    int      used;
    uint32_t level;
    char     title[64];
    char     body[160];
    uint64_t at;                /* when it went up, ms */
};

/* THE HAND-OFF between the accept thread and the drawing one.
 *
 * One producer, one consumer, and a slot is only ever published by setting
 * `used` LAST -- after the text is fully written. The reader sees either a
 * slot it must ignore or one that is complete; it can never see half a
 * message. That is the whole synchronisation, deliberately: a mutex here would
 * be a mutex the drawing loop can block on, and a notifier that can hang the
 * thing it is decorating is worse than no notifier. */
static volatile struct note g_notes[NOTE_MAX];
static volatile int         g_arrived;      /* bumped per accepted message */

static void note_push(uint32_t level, const char *title, const char *body) {
    /* Oldest slot wins when full: the newest message is the one worth showing,
     * and dropping IT to preserve something six seconds old would be backwards. */
    int slot = -1;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < NOTE_MAX; i++) {
        if (!g_notes[i].used) { slot = i; break; }
        if (g_notes[i].at < oldest) { oldest = g_notes[i].at; slot = i; }
    }
    if (slot < 0) slot = 0;

    g_notes[slot].used = 0;                  /* stop anyone reading it mid-write */
    g_notes[slot].level = level;
    snprintf((char *)g_notes[slot].title, sizeof g_notes[slot].title, "%s", title);
    snprintf((char *)g_notes[slot].body,  sizeof g_notes[slot].body,  "%s", body);
    g_notes[slot].at = embk_uptime_ms();
    g_notes[slot].used = 1;                  /* published, last */
    g_arrived++;
}

static int note_count(void) {
    int n = 0;
    for (int i = 0; i < NOTE_MAX; i++) if (g_notes[i].used) n++;
    return n;
}

/* Retire anything that has had its time. */
static void note_expire(void) {
    uint64_t now = embk_uptime_ms();
    for (int i = 0; i < NOTE_MAX; i++)
        if (g_notes[i].used && now - g_notes[i].at > NOTE_MS) g_notes[i].used = 0;
}

/* ---- the accept thread -------------------------------------------------- */

static int g_listen = -1;

static void accept_thread(long arg) {
    (void)arg;
    for (;;) {
        struct emnote_req q;
        uint32_t type = 0;
        unsigned len  = 0;
        int ch = emsvc_accept(g_listen, &type, &q, sizeof q, &len);
        if (ch < 0) continue;

        struct emnote_rep r = { 0 };
        if (type == EMSVC_T_NOTIFY && len >= sizeof q) {
            note_push(q.level, q.title, q.body);
            r.taken = 1;
        }
        /* ANSWERED NOW, not when the banner comes down. The caller has already
         * done whatever it is telling us about. */
        emsvc_reply(ch, type, &r, sizeof r);
    }
}

/* ---- the banners -------------------------------------------------------- */

static int g_place_x;
static int g_placed;
static void placed_reset(void) { g_placed = 0; }

static void banners(void) {
    const struct ui_theme *t = ui_theme();
    note_expire();
    if (note_count() == 0) { placed_reset(); em_app_request_exit(0); return; }
    em_request_frame();                      /* they expire on a clock, not on input */

    /* TOP-RIGHT, under the menu bar. Placed from inside the view because the
     * mover acts on the BOUND window, which only exists once the runtime has
     * created it -- and re-asserted on the first frame of each appearance,
     * since every burst of banners is a new window. */
    if (!g_placed) { g_placed = 1; em_window_move_to(g_place_x, 34); }

    VStack(.width = em_viewport_width(), .height = em_viewport_height(),
           .align = Trailing, .spacing = 8, .padding = 10) {
        static const char *KEY[NOTE_MAX] = { "n0", "n1", "n2", "n3" };
        for (int i = 0; i < NOTE_MAX; i++) {
            if (!g_notes[i].used) continue;
            Color edge = g_notes[i].level == EMNOTE_FAIL ? t->danger
                       : g_notes[i].level == EMNOTE_WARN ? t->warning
                       : t->accent;
            /* THE LEVEL IS A STRIPE, NOT THE BORDER. `.glass` overrides fill
             * AND border (em_apply_box), so a border_color passed alongside it
             * is silently discarded -- the first version asked for an accent
             * edge and got the glass material's own pale outline on every
             * banner, whatever the level. A child box cannot be overridden by
             * its parent's material. */
            HStack(.width = PANEL_W, .align = Fill, .spacing = 0, .clip = 1,
                   .corner = t->radius_lg, .glass = 1, .shadow = 3, .key = KEY[i]) {
                VStack(.width = 4, .background = edge) { }
                VStack(.grow = 1, .spacing = 3, .px = 14, .py = 11, .align = Leading) {
                    Text((const char *)g_notes[i].title).bold();
                    if (g_notes[i].body[0])
                        Text((const char *)g_notes[i].body).caption().secondary();
                }
            }
            /* Click to dismiss: a banner you have read should go when you say
             * so, not when its timer says so. */
            if (Clicked(KEY[i])) g_notes[i].used = 0;
        }
    }
}

static EmApp g_spec = {
    .title      = "Notifications",
    .size       = { PANEL_W + 20, NOTE_MAX * ROW_H + 20 },
    .theme      = Dark,
    .chrome     = Chromeless,
    .material   = Translucent,     /* only the banners paint; the rest is desktop */
    .refresh_ms = 200,             /* they come down on a clock */
    .view       = banners,
};

int main(void) {
    g_listen = emsvc_listen("notify");
    if (g_listen < 0) {
        char b[96];
        snprintf(b, sizeof b, "notifyd: cannot listen at /run/emlink.notify (%d)\n", g_listen);
        embk_puts(1, b);
        return 1;
    }
    if (embk_thread_create(accept_thread, 0) < 0) {
        embk_puts(1, "notifyd: could not start the accept thread\n");
        return 1;
    }
    embk_puts(1, "notifyd: serving /run/emlink.notify\n");

    /* Park the banners in the top-right corner, under the menu bar. */
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    (void)sh;

    for (;;) {
        /* Nothing to show: no window. See the header -- an always-present
         * translucent window would eat every click in that corner. */
        while (note_count() == 0) embk_sleep_ms(120);

        g_place_x = (int)sw - (PANEL_W + 20) - 12;
        em_app_run(&g_spec);       /* returns when the last banner has gone */
    }
}
