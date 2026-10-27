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
 * the corner belongs to whatever is underneath.
 *
 * IN MOTION, ON SPRINGS. A banner slides in from the screen's edge, newest on
 * top; the ones already up glide down to make room, and up again when one
 * goes. It goes back out the way it came -- when its time is up (which does
 * not run while the pointer is on it), when it is clicked, or when it is
 * thrown: dragged sideways it follows the hand, and
 * let go fast enough (or far enough) it flies off at the speed it was thrown;
 * not far or fast enough, it springs home. */

#include <stdio.h>
#include <string.h>

#include "embk.h"
#include "emsvc.h"
#include "emnotify.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "../../../ui/motion/velocity.h"

#define NOTE_MAX     4          /* banners on screen at once */
#define NOTE_MS   6000          /* how long one stays up */
#define PANEL_W    340
#define ROW_H       74
#define EDGE        22          /* from a banner to the screen's right edge */
#define PAD         10          /* the window's own margin, left and top */
#define WIN_W      (PAD + PANEL_W + EDGE)   /* the window reaches the screen's edge */
#define OFF        (PANEL_W + EDGE)         /* a banner this far right is off the screen */

struct note {
    int      used;
    int      leaving;           /* sliding out; the slot is free once it is off */
    uint64_t sequence;
    uint32_t level;
    char     title[64];
    char     body[160];
    uint64_t at;                /* when it went up, ms */
};

/* The accept thread and system-warning poller copy notices under one short
 * mutex. Rendering uses a snapshot, so no UI or IPC work runs under it. */
static embk_mutex note_lock=EMBK_MUTEX_INIT;
static uint64_t note_sequence;
static struct note g_notes[NOTE_MAX];
static volatile int         g_arrived;      /* bumped per accepted message */

static void note_push(uint32_t level, const char *title, const char *body) {
    embk_mutex_lock(&note_lock);
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
    g_notes[slot].leaving = 0;
    g_notes[slot].level = level;
    snprintf((char *)g_notes[slot].title, sizeof g_notes[slot].title, "%s", title);
    snprintf((char *)g_notes[slot].body,  sizeof g_notes[slot].body,  "%s", body);
    g_notes[slot].at = embk_uptime_ms();
    g_notes[slot].sequence=++note_sequence;
    g_notes[slot].used = 1;                  /* published, last */
    g_arrived++;
    embk_mutex_unlock(&note_lock);
}

/* Mirror system warnings without posting them back into the history. */
static void system_notices(void) {
    static uint64_t seen;
    struct embk_notice_info info;
    if(embk_notify_get(&info,0))return;
    for(unsigned i=info.count;i>0;i--) {
        const struct embk_notice *n=&info.entries[i-1];
        if(n->sequence>seen && !n->source_pid)note_push(n->severity,n->title,n->body);
    }
    if(info.latest>seen)seen=info.latest;
}
static int note_count(void) {
    embk_mutex_lock(&note_lock);
    int n = 0;
    for (int i = 0; i < NOTE_MAX; i++) if (g_notes[i].used) n++;
    embk_mutex_unlock(&note_lock);
    return n;
}

/* Send anything that has had its time on its way out. */
static void note_expire(void) {
    embk_mutex_lock(&note_lock);
    uint64_t now = embk_uptime_ms();
    for (int i = 0; i < NOTE_MAX; i++)
        if (g_notes[i].used && now - g_notes[i].at > NOTE_MS) g_notes[i].leaving = 1;
    embk_mutex_unlock(&note_lock);
}

/* Slot `i`, if it still holds notice `sequence`: kept up (its time starts
 * again -- one being read does not go), on its way out, or gone. */
static void note_keep(int i, uint64_t sequence) {
    embk_mutex_lock(&note_lock);
    if (g_notes[i].sequence == sequence && !g_notes[i].leaving) g_notes[i].at = embk_uptime_ms();
    embk_mutex_unlock(&note_lock);
}
static void note_leave(int i, uint64_t sequence) {
    embk_mutex_lock(&note_lock);
    if (g_notes[i].sequence == sequence) g_notes[i].leaving = 1;
    embk_mutex_unlock(&note_lock);
}
static void note_gone(int i, uint64_t sequence) {
    embk_mutex_lock(&note_lock);
    if (g_notes[i].sequence == sequence) g_notes[i].used = 0;
    embk_mutex_unlock(&note_lock);
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
        if (type == EMSVC_T_NOTIFY && len >= sizeof q && q.level<=EMNOTE_FAIL) {
            q.title[sizeof q.title-1]=0;q.body[sizeof q.body-1]=0;
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
static int g_placed, g_frames;
static void placed_reset(void) { g_placed = 0; g_frames = 0; }

/* What each slot's banner measured last frame, and where it was put: a
 * banner going out keeps its place while the rest close up behind it. */
static float    g_h[NOTE_MAX], g_y[NOTE_MAX];
static uint64_t g_hseq[NOTE_MAX];

/* THE HAND ON A BANNER. Pressed, it follows the pointer sideways -- freely
 * to the right, the way out, and only a little to the left, where there is
 * nowhere to go (a rubber band). Let go, it is thrown. */
static struct { uint64_t sequence; int on, moved; float x0, dx; struct vel_track v; } g_hand;
static float hand_x(float dx) { return dx >= 0 ? dx : -24.f * (1.f - 1.f / (-dx / 60.f + 1.f)); }

static void banners(void) {
    const struct ui_theme *t = ui_theme();
    system_notices();
    note_expire();
    if (note_count() == 0) { placed_reset(); em_app_request_exit(0); return; }
    em_request_frame();                      /* they expire on a clock, not on input */

    /* TOP-RIGHT, under the menu bar, the window's right edge on the screen's:
     * a banner slides in from the edge of the screen, not from a line short of
     * it. Placed from inside the view because the mover acts on the BOUND
     * window, which only exists once the runtime has created it -- and
     * re-asserted on the first frame of each appearance, since every burst of
     * banners is a new window. */
    if(!g_placed) {
        g_placed=1;struct embk_desktop desktop={.version=1};embk_desktop_control(EMBK_DESKTOP_GET,&desktop);
        em_window_move_to(g_place_x,(int)desktop.top+8);
    }

    struct note notes[NOTE_MAX];
    embk_mutex_lock(&note_lock);memcpy(notes,g_notes,sizeof notes);embk_mutex_unlock(&note_lock);
    /* newest on top: slots in falling sequence */
    int order[NOTE_MAX], n = 0;
    for (int i = 0; i < NOTE_MAX; i++) if (notes[i].used) order[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && notes[order[b]].sequence > notes[order[b - 1]].sequence; b--) {
            int k = order[b]; order[b] = order[b - 1]; order[b - 1] = k;
        }
    float y_next = PAD;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        if (g_hseq[i] != notes[i].sequence) { g_hseq[i] = notes[i].sequence; g_h[i] = ROW_H - 8; g_y[i] = y_next; }
        if (!notes[i].leaving) { g_y[i] = y_next; y_next += g_h[i] + 8; }
    }

    uint64_t now_ns = embk_uptime_ms() * 1000000ull, released = 0;
    /* A NEW WINDOW'S FIRST FRAMES ARE SLOW to reach the screen (its fonts, its
     * first full render): a slide started on the first of them was mostly over
     * before anything could be seen. The banners wait off the edge until the
     * window has been shown, then come in. */
    int warming = g_frames++ < 2;
    VStack(.width = em_viewport_width(), .height = em_viewport_height()) {
        static const char *KEY[NOTE_MAX] = { "n0", "n1", "n2", "n3" };
        for (int k = n - 1; k >= 0; k--) {      /* oldest first: the newest is drawn over */
            int i = order[k];
            uint64_t seq = notes[i].sequence;
            char xid[24], yid[24];
            snprintf(xid, sizeof xid, "nx%llu", (unsigned long long)seq);
            snprintf(yid, sizeof yid, "ny%llu", (unsigned long long)seq);
            Color edge = notes[i].level == EMNOTE_FAIL ? t->danger
                       : notes[i].level == EMNOTE_WARN ? t->warning
                       : t->accent;
            /* THE LEVEL IS A STRIPE, NOT THE BORDER. `.glass` overrides fill
             * AND border (em_apply_box), so a border_color passed alongside it
             * is silently discarded -- the first version asked for an accent
             * edge and got the glass material's own pale outline on every
             * banner, whatever the level. A child box cannot be overridden by
             * its parent's material. */
            HStack(.width = PANEL_W, .align = Fill, .spacing = 0, .clip = 1,
                   .corner = t->radius_lg, .glass = 1, .shadow = 3, .key = KEY[i]) {
                float rx, ry, rw, rh;
                if (ui_open_rect(&rx, &ry, &rw, &rh) && rh > 0) g_h[i] = rh;
                if (ui_is_hovered()) note_keep(i, seq);   /* under the pointer: being read */
                float px, py; ui_pointer_pos(&px, &py); (void)py;
                float x;
                if (ui_is_active() && !notes[i].leaving) {
                    if (!g_hand.on || g_hand.sequence != seq) {
                        g_hand.on = 1; g_hand.sequence = seq; g_hand.moved = 0;
                        g_hand.x0 = px; vel_reset(&g_hand.v);
                    }
                    g_hand.dx = px - g_hand.x0;
                    if (g_hand.dx > 6 || g_hand.dx < -6) g_hand.moved = 1;
                    vel_add(&g_hand.v, now_ns, (int32_t)px, 0);
                    x = hand_x(g_hand.dx);
                    em_spring_throw(xid, x, 0);      /* the spring is where the hand is */
                } else {
                    if (g_hand.on && g_hand.sequence == seq) {
                        /* LET GO: far or fast enough to the right, it is
                         * thrown off at the speed it had; a press that never
                         * moved is a click, which dismisses too */
                        g_hand.on = 0; released = seq;
                        int32_t vx, vy; vel_get(&g_hand.v, now_ns, &vx, &vy); (void)vy;
                        float at = hand_x(g_hand.dx);
                        em_spring_throw(xid, at, (float)vx);
                        if (!g_hand.moved || at > PANEL_W / 3 || vx > 600) {
                            note_leave(i, seq); notes[i].leaving = 1;
                        }
                    }
                    if (warming) em_spring_throw(xid, OFF, 0);
                    x = em_spring(xid, OFF, notes[i].leaving || warming ? OFF : 0, EmSmooth);
                    if (notes[i].leaving && x >= OFF - 1) note_gone(i, seq);
                }
                float y = em_spring(yid, g_y[i], g_y[i], EmSmooth);
                ui_set_overlay(true);
                ui_set_offset(PAD + x, y);
                VStack(.width = 4, .background = edge) { }
                VStack(.grow = 1, .spacing = 3, .px = 14, .py = 11, .align = Leading) {
                    Text((const char *)notes[i].title).bold();
                    if (notes[i].body[0])
                        Text((const char *)notes[i].body).caption().secondary();
                }
            }
            /* A click too quick for a frame to see it held is still a click:
             * dismissed, unless the hand's own release was just handled */
            if (Clicked(KEY[i]) && released != seq) note_leave(i, seq);
        }
    }
}

static EmApp g_spec = {
    .title      = "Notifications",
    .size       = { WIN_W, NOTE_MAX * ROW_H + 20 },
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
        while (note_count() == 0) {system_notices();embk_sleep_ms(120);}

        g_place_x = (int)sw - WIN_W;
        em_app_run(&g_spec);       /* returns when the last banner has gone */
    }
}
