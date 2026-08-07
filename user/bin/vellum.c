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
#include "oscfg.h"   /* the user's dark/light choice answers prefers-color-scheme */
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "render.h"
#include "css.h"
#include "imgcache.h"
#include "jsdom.h"
#include "form.h"

/* who owns a fetch on the shared worker */
#define DOC_TAG 1
#include "url.h"
#include "net.h"
#include "fetchjob.h"
#include "select.h"
#include "cssref.h"

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
static struct css_sheet g_sheet;   /* the page's own <style>, cascaded */


/* The cascade is built from EVERY sheet the page has: the external ones that
 * have arrived so far, then the document's own <style>. Rebuilt each time a
 * <link> lands, because css_sheet_parse replaces a sheet rather than extending
 * it -- and because a page must be readable before the last stylesheet does. */
static char g_allcss[160 * 1024];
/* The width a media query is evaluated against: the document's content box,
 * which is the window minus the ScrollView's padding -- what the page actually
 * gets to lay out in. */
static float sheet_viewport_w(void) { return em_viewport_width() - 44.0f; }

static void rebuild_sheet(void) {
    struct oscfg cfg; oscfg_load(&cfg);
    css_media_set(sheet_viewport_w(), em_viewport_height() - 132.0f, cfg.dark != 0);
    size_t n = 0, extn = 0;
    const char *ext = cssref_text(&extn);
    if (ext && extn) {
        if (extn > sizeof g_allcss - 2) extn = sizeof g_allcss - 2;
        memcpy(g_allcss, ext, extn);
        n = extn;
        g_allcss[n++] = '\n';
    }
    if (g_doc.css && g_doc.css_len) {
        size_t k = g_doc.css_len;
        if (n + k > sizeof g_allcss - 1) k = sizeof g_allcss - 1 - n;
        memcpy(g_allcss + n, g_doc.css, k);
        n += k;
    }
    g_allcss[n] = 0;
    css_sheet_parse(&g_sheet, n ? g_allcss : 0, n);
}
static int              g_root = -1;

static char  g_url[512]   = "";
static char  g_bar[512]   = "";      /* what the URL field is showing        */
static char  g_status[256] = "";
static char  g_console[256] = "";     /* the page's last console.log */
static char  g_status_done[256] = ""; /* what the status said before loading */
static int   g_status_busy;

/* What the status line says about the load that produced this page. Kept as
 * FIELDS rather than composed once, because the counts it reports keep
 * changing after the document lands: an external stylesheet arriving adds
 * rules. The line said "1 css rule" on a page with four while three of them
 * were already applied on screen -- a status line that under-reports is the
 * same lie as one that over-reports. */
static struct {
    int    status;
    char   via[64];
    size_t bytes;
    int    res_trunc;
} g_st;

static void update_status(void) {
    char css[80]; css[0] = 0;
    if (g_sheet.n) snprintf(css, sizeof css, "  %d css rule%s%s", g_sheet.n,
                            g_sheet.n == 1 ? "" : "s", g_sheet.truncated ? "+" : "");
    if (cssref_pending()) {
        size_t k = strlen(css);
        snprintf(css + k, sizeof css - k, " (+css)");
    }
    if (g_doc.n_js) {
        size_t k = strlen(css);
        snprintf(css + k, sizeof css - k, "  %d script%s",
                 g_doc.n_js, g_doc.n_js == 1 ? "" : "s");
    }
    snprintf(g_status, sizeof g_status, "%d  %s  %zu bytes  %d nodes%s%s%s",
             g_st.status, g_st.via, g_st.bytes, g_doc.n, css,
             g_st.res_trunc  ? "  (response truncated)" : "",
             g_doc.truncated ? "  (document truncated)" : "");
    snprintf(g_status_done, sizeof g_status_done, "%s", g_status);
}
static float g_scroll = 0;

/* Back/forward, the same shape Files uses -- "back" alone is half a history. */
static char g_back[24][512]; static int g_back_n;
static char g_fwd[24][512];  static int g_fwd_n;

/* A navigation requested by a click, acted on AFTER the frame: the href lives
 * in the arena the load is about to overwrite. */
static char g_goto[512] = "";
static void on_link(const char *href) { (void)href; }

/* A script's output has to go SOMEWHERE a person can see, or console.log is a
 * call that does nothing observable -- the exact thing this browser refuses to
 * ship elsewhere. The status line is where the browser already tells the truth
 * about a page, so it is where a page's own words go too. */
/* A click reached an element a script is listening to. The handler may rewrite
 * the document, so the dirty flag is checked right after -- that is what makes
 * a button on a page actually change the page. */
/* A POST is a navigation that carries something. It shares the history and
 * load path with an ordinary one -- a form submission IS a page visit -- and
 * differs only in what goes on the wire. Declared up here because `load` is
 * what consults them and it comes first. */
static char g_post[1024];
static int  g_have_post;
static void navigate(const char *url);
static void navigate_post(const char *url, const char *body);

/* Submitting is just navigating, which is the whole reason a form is not a
 * special case in this browser: build the URL (and body, for POST) and hand it
 * to the same load path a link uses. */
static void on_submit(int node) {
    char url[512], body[1024];
    int how = form_submit(&g_doc, node, g_url, url, sizeof url, body, sizeof body);
    if (how == 1)      navigate(url);
    else if (how == 2) navigate_post(url, body);
}

static void on_dom_click(int node) {
    if (jsdom_dispatch_click(node)) em_request_frame();
}

static void on_console(const char *line) {
    snprintf(g_console, sizeof g_console, "%s", line);
}

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
    cssref_reset(); rebuild_sheet();
    imgcache_reset();
    vsel_reset();
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
    int started = g_have_post
        ? fetchjob_start_post(url, g_post, g_incoming, sizeof g_incoming, DOC_TAG)
        : fetchjob_start(url, g_incoming, sizeof g_incoming, DOC_TAG);
    g_have_post = 0;           /* one submission: a later link must not re-post */
    if (started != 0) {
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
    /* the author's stylesheet, borrowed from the document arena (which is why
     * it is parsed here, once, and not per frame) */
    cssref_start(&g_doc, g_url);
    rebuild_sheet();
    imgcache_reset();          /* one page's pictures never leak into the next */
    vsel_reset();              /* ...nor does a selection: it indexed the OLD words */
    form_reset();              /* ...nor one page's typing into the next */

    /* A NEW WORLD per page: the engine is torn down and rebuilt, so a script
     * cannot outlive the document that wrote it, and one page's globals can
     * never be read by the next. Then run what the page brought. */
    g_console[0] = 0;
    jsdom_set_console(on_console);
    jsdom_set_url(g_url);
    if (jsdom_open(&g_doc, &g_sheet) == 0 && g_doc.n_js > 0) {
        int failed = jsdom_run_scripts();
        jsdom_take_dirty();     /* the first render happens anyway */
        if (failed && !g_console[0])
            snprintf(g_console, sizeof g_console, "%d script(s) threw", failed);
    }

    g_st.status = res->status;
    snprintf(g_st.via, sizeof g_st.via, "%s", res->via);
    g_st.bytes = n;
    g_st.res_trunc = res->truncated;
    update_status();
}

static void navigate_post(const char *url, const char *body) {
    snprintf(g_post, sizeof g_post, "%s", body ? body : "");
    g_have_post = 1;
    navigate(url);
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

/* Keyboard paging: Space a page down, 'b' a page up -- every browser's oldest
 * shortcut, and it scrolls without touching the wheel at all. (It also makes
 * scrolling drivable from the test harness, where wheel events cannot be
 * synthesized -- a feature and an instrument in one.) Consumed only while no
 * text field has focus, or typing a space in the URL bar would jump the page. */
/* Selection: a drag across the document, and a way to take it away. The drag
 * is tracked here rather than in select.c because the press/release EDGE is an
 * app-loop fact -- select.c is told what happened, not asked to guess. */
static bool g_ptr_was_down;

static void selection_tick(void) {
    float px, py;
    ui_pointer_pos(&px, &py);
    bool down = ui_pointer_down();
    int changed = 0;
    if (down && !g_ptr_was_down)      changed = vsel_pointer(px, py, 1, 1);
    else if (down)                    changed = vsel_pointer(px, py, 0, 1);
    else if (g_ptr_was_down)          changed = vsel_pointer(px, py, 0, 0);
    g_ptr_was_down = down;
    if (changed) em_request_frame();
}

static int vellum_key(int ch) {
    if (ch == 0x03) {                     /* Ctrl+C */
        static char sel[16384];
        size_t n = vsel_copy_text(sel, sizeof sel);
        if (n) embk_clip_set(sel, n);
        if (n) snprintf(g_status, sizeof g_status, "Copied %u bytes", (unsigned)n);
        else   snprintf(g_status, sizeof g_status, "Nothing selected");
        snprintf(g_status_done, sizeof g_status_done, "%s", g_status);
        return 1;
    }
    if (ch == 0x01) { return vsel_all(); }   /* Ctrl+A: the whole document */
    if (ch == 27 && vsel_clear()) return 1;  /* Esc drops a selection first */
    /* Enter in a form field submits it -- a search box you cannot submit from
     * the keyboard feels broken, and every browser has behaved this way since
     * forms existed. */
    if (ch == '\n' || ch == '\r') {
        int f = vellum_focused_field();
        if (f >= 0) { on_submit(f); return 1; }
    }
    if (ui_any_focus()) return 0;
    float page = (em_viewport_height() - 132.0f) * 0.85f;
    if (ch == ' ')      { g_scroll += page; }
    else if (ch == 'b') { g_scroll -= page; }
    else return 0;
    if (g_scroll < 0) g_scroll = 0;
    return 1;
}

/* --- the window --------------------------------------------------------- */

static void app(void) {
    static bool first = true;
    if (first) {
        first = false;
        em_set_key_hook(vellum_key);
        em_set_post_layout_hook(vsel_sync_geometry);
        vellum_set_link_handler(on_link);
        vellum_set_event_hooks(jsdom_has_listener, on_dom_click);
        vellum_set_submit_handler(on_submit);
        const char *start = getenv("VELLUM_URL");
        navigate(start && start[0] ? start : "/system/web/index.html");
    }
    /* Has the worker landed? Polled once per frame, which is the whole cost of
     * not freezing. */
    struct vnet_result res;
    if (fetchjob_poll(DOC_TAG, &res) == 1) finish_load(&res);

    /* Stylesheets BEFORE pictures. Both share the one worker, and a page that
     * paints its images before it knows what colour anything is shows the
     * reader a wrong-looking page and then rearranges it. Style first is also
     * the order a browser's own preload scanner uses, for the same reason. */
    if (cssref_pump()) { rebuild_sheet(); update_status(); em_request_frame(); }
    /* A RESIZE changes what the media queries answer, and the sheet was parsed
     * against the old width. Re-parse when the viewport actually moves --
     * otherwise a window dragged past a breakpoint keeps the other layout. */
    { static float last_w; float w = sheet_viewport_w();
      if (w != last_w) { last_w = w; rebuild_sheet(); em_request_frame(); } }

    /* ...and the page's pictures, one at a time on the same worker. Each one
     * that lands changes the page, so ask for a frame. */
    if (!cssref_pending() && imgcache_pump()) em_request_frame();
    /* One pump for everything the engine owes the page: due timers, a landed
     * fetch, and the microtask queue promises resolve onto. Before the dirty
     * check, because a handler is the most likely thing to have changed the
     * document. */
    if (jsdom_pump(embk_uptime_ms())) em_request_frame();
    if (jsdom_take_dirty()) em_request_frame();
    /* Keep frames coming while the page has WORK OUTSTANDING -- a timer to
     * fire or a fetch to land -- and stop the moment it does not, so an idle
     * page costs nothing. */
    if (jsdom_next_timer() || jsdom_busy()) em_app_set_refresh(60);
    else if (!fetchjob_busy() && !imgcache_pending() && !cssref_pending())
        em_app_set_refresh(-1);
    if (imgcache_pending() || cssref_pending()) em_app_set_refresh(200);

    /* While a fetch is in flight the view has to keep being built, or the
     * runtime -- which draws on input by design -- would never poll again and
     * the page would land invisibly. A periodic tick, not a per-frame request:
     * five a second is plenty to notice a fetch landing, and it leaves the CPU
     * to the thing the user is actually waiting for. */
    static bool ticking = false;
    bool busy_now = fetchjob_busy() != 0;
    if (busy_now != ticking) { ticking = busy_now; em_app_set_refresh(busy_now ? 200 : -1); }

    /* Deliberately NOT forcing a full repaint here. The app renders straight
     * into the shared window buffer, so a full repaint begins by CLEARING the
     * pixels the compositor is showing -- and while a crypto-heavy worker has
     * the core, the redraw that follows takes long enough that an empty window
     * is what the user actually sees. Leaving the old pixels alone means the
     * page stays readable and only what changed is overwritten. */

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
        g_status_busy = 1;
    } else if (g_status_busy) {
        /* ...and PUT IT BACK when the loading stops. Leaving "Loading..." on
         * screen after everything has arrived is a status line that lies --
         * and it lied for a whole minute while an image that could never load
         * was quietly failing, which is exactly when a person is reading it. */
        g_status_busy = 0;
        snprintf(g_status, sizeof g_status, "%s", g_status_done);
    }

    selection_tick();

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
                const char *clicked = vellum_render_sized(&g_doc, g_root, &g_sheet, g_url,
                                                         em_viewport_width() - 44.0f);
                /* A DRAG that happens to end on a link is a selection, not a
                 * click. Without this, selecting a paragraph that contains a
                 * link navigates away the moment you let go -- and the text you
                 * just selected is gone with the page. */
                if (clicked && !vsel_active())
                    snprintf(g_goto, sizeof g_goto, "%s", clicked);
            }
        }

        Divider();
        HStack(.spacing = 10, .align = Center, .px = 12, .py = 4) {
            /* what the PAGE said outranks what the browser has to say: a
             * script's output is the thing the reader is waiting for. */
            const char *h = vellum_hovered_link();
            Text(h ? h : (g_console[0] ? g_console : g_status)).caption().tertiary();
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
