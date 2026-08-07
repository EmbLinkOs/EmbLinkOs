/* vellum.c -- EmbLink's browser. See docs/BROWSER.md.
 *
 * B1: the pipeline end to end, minus the network. Load a document from the
 * filesystem, parse it, style it with the user-agent stylesheet, render it,
 * and follow links between local pages. Everything except where the bytes come
 * from -- which is exactly the seam the design put between fetch and parse, so
 * B2 changes one function and nothing else.
 *
 * The chrome follows the house style: AppBar with the lights leading, the
 * title centred, the app's controls trailing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "render.h"
#include "url.h"
#include "net.h"
#include "fetchjob.h"

/* One document at a time, in fixed arenas. A browser that can be handed a
 * hostile page needs a bounded appetite -- see docs/BROWSER.md §7. */
#define SRC_MAX   (512 * 1024)
#define NODE_MAX  8192
#define STR_MAX   (256 * 1024)

static char             g_src[SRC_MAX];      /* the document being DISPLAYED */
/* ...and a second buffer for the one being FETCHED. They cannot be the same
 * buffer: html_parse stores pointers INTO the source, so every text node of
 * the page on screen points into g_src. A worker writing the next response
 * there dissolves the current page under the renderer -- the window went blank
 * mid-fetch, which looked like a repaint bug and was not. The bytes only move
 * across once the fetch is finished and nothing is reading the old ones. */
static char             g_incoming[SRC_MAX];
static struct html_node g_nodes[NODE_MAX];
static char             g_strs[STR_MAX];
static struct html_doc  g_doc;
static int              g_root = -1;

static char  g_url[512]   = "";
static char  g_bar[512]   = "";      /* what the URL field is showing        */
static char  g_status[256] = "";
static float g_scroll = 0;

/* Back/forward, the same shape Files uses -- "back" alone is half a history. */
static char g_back[24][512]; static int g_back_n;
static char g_fwd[24][512];  static int g_fwd_n;

/* A navigation requested by a click, acted on AFTER the frame: the href lives
 * in the arena the load is about to overwrite. */
static char g_goto[512] = "";
static void on_link(const char *href) { (void)href; }

/* --- loading ------------------------------------------------------------ */

/* B2: this is now one call. Where the bytes come from -- EMBKFS, a socket, a
 * TLS session -- is net.c's business, and the seam the design put here is the
 * reason the browser above it did not have to change. */

/* The error page. A browser that shows a blank window on failure is a browser
 * you cannot debug -- so failures are DOCUMENTS, and go through exactly the
 * same parse/style/render path as any other page. */
static void load_error(const char *url, const char *why) {
    snprintf(g_src, sizeof g_src,
             "<h1>Cannot open this page</h1>"
             "<p>%s</p><p><b>%s</b></p>"
             "<p>Vellum reads documents from the filesystem in this build. "
             "Try <a href=\"/system/web/index.html\">the start page</a>.</p>",
             why, url);
    g_root = html_parse(&g_doc, g_src, strlen(g_src), g_nodes, NODE_MAX, g_strs, STR_MAX);
    snprintf(g_status, sizeof g_status, "%s", why);
}

/* Starting a load no longer BLOCKS. A TLS handshake to a real host is several
 * round trips and each one is a frame the window does not draw -- from the
 * outside that is an application that has died. The fetch runs on a worker
 * (fetchjob.c) and the view keeps drawing, showing what it is waiting for. */
static void load(const char *url) {
    snprintf(g_url, sizeof g_url, "%s", url);
    snprintf(g_bar, sizeof g_bar, "%s", url);
    g_scroll = 0;
    if (fetchjob_start(url, g_incoming, sizeof g_incoming) != 0) {
        /* One at a time. Refusing is honest: the page you asked for first is
         * still coming, and silently dropping it would be worse. */
        snprintf(g_status, sizeof g_status, "Still loading %s", fetchjob_url());
        return;
    }
    snprintf(g_status, sizeof g_status, "Loading...  0.0s");
    (void)url;
}

/* The other half of load(), run when the bytes actually arrive. */
static void finish_load(const struct vnet_result *res) {
    if (res->err[0] && res->len == 0) {
        load_error(g_url, res->err);
        return;
    }
    /* A redirect changes where you ARE, and the address bar has to say so. */
    if (res->redirects) {
        snprintf(g_url, sizeof g_url, "%s", res->final_url);
        snprintf(g_bar, sizeof g_bar, "%s", res->final_url);
    }

    /* Now, and only now, is it safe: the fetch is done, so nothing is writing
     * g_incoming and nothing is reading the old g_src any more. */
    size_t n = res->len < sizeof g_src - 1 ? res->len : sizeof g_src - 1;
    memcpy(g_src, g_incoming, n);
    g_src[n] = 0;
    g_root = html_parse(&g_doc, g_src, n, g_nodes, NODE_MAX, g_strs, STR_MAX);
    if (g_root < 0) { load_error(g_url, "The document could not be parsed."); return; }

    /* The status line is the browser's honesty: how the bytes arrived, how many
     * there were, how long it took, and whether we told the truth about all
     * of them. */
    snprintf(g_status, sizeof g_status, "%d  %s  %zu bytes  %d nodes%s%s",
             res->status, res->via, n, g_doc.n,
             res->truncated   ? "  (response truncated)" : "",
             g_doc.truncated  ? "  (document truncated)" : "");
}

static void navigate(const char *url) {
    if (!url || !url[0]) return;
    if (g_url[0]) {
        if (g_back_n == 24) { memmove(g_back, g_back + 1, sizeof g_back - sizeof g_back[0]); g_back_n--; }
        snprintf(g_back[g_back_n++], sizeof g_back[0], "%s", g_url);
    }
    g_fwd_n = 0;
    load(url);
}

static void go_back(void) {
    if (!g_back_n) return;
    if (g_fwd_n < 24) snprintf(g_fwd[g_fwd_n++], sizeof g_fwd[0], "%s", g_url);
    char to[512]; snprintf(to, sizeof to, "%s", g_back[--g_back_n]);
    load(to);
}
static void go_fwd(void) {
    if (!g_fwd_n) return;
    if (g_back_n < 24) snprintf(g_back[g_back_n++], sizeof g_back[0], "%s", g_url);
    char to[512]; snprintf(to, sizeof to, "%s", g_fwd[--g_fwd_n]);
    load(to);
}

/* --- the window --------------------------------------------------------- */

static void app(void) {
    static bool first = true;
    if (first) {
        first = false;
        vellum_set_link_handler(on_link);
        const char *start = getenv("VELLUM_URL");
        navigate(start && start[0] ? start : "/system/web/index.html");
    }
    /* Has the worker landed? Polled once per frame, which is the whole cost of
     * not freezing. */
    struct vnet_result res;
    if (fetchjob_poll(&res) == 1) finish_load(&res);

    /* While a fetch is in flight the view has to keep being built, or the
     * runtime -- which only draws on input by design -- would never poll again
     * and the page would land invisibly.
     *
     * THROTTLED, though. Asking for a frame every iteration rebuilds and
     * re-renders the whole document as fast as the loop can go, and on a single
     * core it does that against a worker running a TLS handshake: the two
     * starve each other and the window paints in pieces. Five polls a second is
     * far more than enough to notice a fetch landing, and it leaves the CPU to
     * the thing the user is actually waiting for. */
    if (fetchjob_busy()) {
        static uint64_t last_poll_ms;
        uint64_t now = embk_uptime_ms();
        if (now - last_poll_ms >= 200) { last_poll_ms = now; em_request_frame(); }
    }

    /* The progress report goes in the status line that is ALREADY THERE, and
     * that is a deliberate design choice rather than a compromise. A row that
     * appears and disappears moves every row below it, and the runtime's
     * incremental repaint paints the new layout over the old pixels -- a
     * dedicated "loading" strip made the window look corrupted for the whole
     * fetch. Changing a STRING changes no geometry, so there is nothing to get
     * out of step. The ticking tenths are a better liveness signal than a
     * spinner anyway: they prove the UI thread is running, which is the exact
     * thing the user could not tell before. */
    if (fetchjob_busy()) {
        /* Elapsed FIRST. The URL is already in the address bar two rows up, and
         * putting it here as well pushed the one piece of information that is
         * actually changing off the end of the line. */
        unsigned ms = fetchjob_elapsed_ms();
        snprintf(g_status, sizeof g_status, "Loading...  %u.%us", ms / 1000, (ms % 1000) / 100);
    }

    /* act on last frame's click before building this one */
    if (g_goto[0]) {
        /* Resolve against where we ARE. A relative href in a page fetched over
         * the network means a network location, and the same href in a local
         * document means the file beside it -- one rule, two worlds. */
        char u[512];
        if (url_resolve(g_url, g_goto, u, sizeof u) != 0)
            snprintf(u, sizeof u, "%s", g_goto);
        g_goto[0] = 0;
        navigate(u);
    }

    Window("Vellum") {
        AppBar("Vellum") {
            if (IconButton(IconChevronL).clicked()) go_back();
            if (IconButton(IconChevronR).clicked()) go_fwd();
            if (IconButton(IconArrowR).clicked())   load(g_url);
        }

        /* the address row: the widest thing in the chrome, as it should be */
        HStack(.spacing = 8, .align = Center, .px = 12, .py = 6) {
            if (TextField(g_bar, sizeof g_bar, "Path or URL").focused()) { }
            if (Button("Open").primary().font(Caption).py(2).clicked()) navigate(g_bar);
        }
        Divider();

        ScrollView(&g_scroll, em_viewport_height() - 132.0f) {
            /* Fill, not Leading: this is the block every other block inherits
             * its width from. Left it Leading and the whole document sizes to
             * its longest line instead of to the window, so nothing wraps. */
            VStack(.spacing = 0, .align = Fill, .padding = 22, .grow = 1) {
                const char *clicked = vellum_render(&g_doc, g_root);
                if (clicked) snprintf(g_goto, sizeof g_goto, "%s", clicked);
            }
        }

        Divider();
        HStack(.spacing = 10, .align = Center, .px = 12, .py = 4) {
            const char *h = vellum_hovered_link();
            Text(h ? h : g_status).caption().tertiary();
            Spacer();
            Text(g_url).caption().tertiary();
        }
    }
}

EM_APPLICATION {
    .title  = "Vellum",
    .size   = { 940, 620 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
