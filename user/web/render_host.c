/* user/web/render_host.c -- run the browser's render path on the HOST.
 *
 * Everything from the document bytes to resolved pixel geometry is plain
 * userland C with no syscalls in it: html.c parses, style.c computes, render.c
 * emits EmUI nodes, and layout resolves them. Only the window and the network
 * need the OS. So the entire pipeline can be driven from a host `main`, which
 * is what this file is -- and a bug that takes a five-minute boot to look at
 * takes two seconds to look at here.
 *
 * It prints the resolved tree (kind, absolute rect, text) and optionally
 * renders a PNG, so "is the layout wrong or is the emission wrong?" is a
 * question you answer by reading, not by squinting at a screenshot.
 *
 *   make browser-render                     -- the start page
 *   make browser-render DOC=path W=940      -- any document, any width
 */
#include "em.h"
#include "scene_render.h"
#include "font.h"
#include "html.h"
#include "style.h"
#include "render.h"
#include "css.h"
#include "imgcache.h"
#include "png.h"
#include "jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 0; }
    fclose(f); buf[n] = 0; *len = (size_t)n; return buf;
}

/* ---- the document ------------------------------------------------------ */

#define NODE_MAX 8192
#define STR_MAX  (256 * 1024)
static struct html_node g_nodes[NODE_MAX];
static char             g_strs[STR_MAX];
static struct html_doc  g_doc;
static int              g_root = -1;
static float            g_scroll = 0;
static struct css_sheet g_sheet;
static int HDEFER;   /* host cache: pretend no picture has arrived yet */
static const char      *g_doc_base;

/* The app's shape, verbatim from user/bin/vellum.c -- if this diverges the
 * harness stops being evidence. `g_busy` stands in for a fetch in flight, so
 * the loading strip can be laid out here instead of in a five-minute boot. */
static int  g_busy;
static char g_bar[512] = "https://valid-isrgrootx1.letsencrypt.org/";

static void app(void) {
    Window("Vellum") {
        AppBar("Vellum") {
            IconButton(IconChevronL);
            IconButton(IconChevronR);
            IconButton(IconArrowR);
        }
        HStack(.spacing = 8, .align = Center, .px = 12, .py = 6) {
            TextField(g_bar, sizeof g_bar, "Path or URL");
            Button("Open").primary().font(Caption).py(2);
        }
        Divider();

        if (g_busy) {
            HStack(.spacing = 8, .align = Center, .px = 12, .py = 4) {
                Spinner();
                Text("Loading https://valid-isrgrootx1.letsencrypt.org/   1.2s")
                    .caption().secondary();
            }
            Divider();
        }

        ScrollView(&g_scroll, em_viewport_height() - (g_busy ? 164.0f : 132.0f)) {
            VStack(.spacing = 0, .align = Fill, .padding = 22, .grow = 1) {
                vellum_render_sized(&g_doc, g_root, &g_sheet, g_doc_base,
                                    em_viewport_width() - 44.0f);
            }
        }

        Divider();
        HStack(.spacing = 10, .align = Center, .px = 12, .py = 4) {
            Text("200  https (authenticated)  4067 bytes").caption().tertiary();
            Spacer();
            Text(g_bar).caption().tertiary();
        }
    }
}

/* ---- the dump ---------------------------------------------------------- */

static struct scene_arena  sa;
static struct layout_arena la;

/* The lowest text INSIDE THE SCROLL AREA, and how many text nodes are there.
 *
 * Inside, specifically: the first attempt walked the whole window and kept
 * finding y=688.6 on every page, because the lowest text in a browser window
 * is the status bar -- furniture that never moves no matter what the document
 * does. A reflow check that measures the chrome cannot fail, which is the
 * worst property a check can have. The document lives under the first
 * clipping node (the ScrollView), so the walk only counts once it is inside
 * one. Layout positions everything whether or not it is clipped away, so text
 * below the fold still reports an honest y. */
static void text_extent(struct node_handle h, float oy, int inside,
                        float *maxy, int *count) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float y = oy + n->ty;
    if (n->clip_children) inside = 1;
    if (inside && n->kind == SCENE_NODE_TEXT) {
        if (y > *maxy) *maxy = y;
        (*count)++;
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        text_extent(c, y, inside, maxy, count);
        c = cn->next_sibling;
    }
}

static const char *kindname(enum scene_node_kind k) {
    switch (k) {
    case SCENE_NODE_GROUP: return "group";
    case SCENE_NODE_RECT:  return "rect ";
    case SCENE_NODE_IMAGE: return "image";
    case SCENE_NODE_TEXT:  return "TEXT ";
    }
    return "?";
}

/* Walk the SCENE tree after layout: every node already carries its resolved
 * size and its offset from its parent, so absolute position is just the sum
 * down the spine. */
static void dump(struct node_handle h, float ox, float oy, int depth, int maxdepth) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float x = ox + n->tx, y = oy + n->ty;

    if (depth <= maxdepth) {
        printf("%*s%s %7.1f,%-7.1f %6.1fx%-6.1f", depth * 2, "", kindname(n->kind), x, y,
               n->width, n->height);
        if (n->kind == SCENE_NODE_TEXT && n->data.text.utf8)
            printf("  \"%s\"", n->data.text.utf8);
        else if (n->clip_children)
            printf("  [clip]");
        printf("\n");
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        dump(c, x, y, depth + 1, maxdepth);
        c = cn->next_sibling;
    }
}

/* The question this harness was built to answer: how many wrapping rows does
 * one paragraph become, and where does each one sit? A paragraph that renders
 * as three lines should be ONE row node whose children wrap inside it. If it
 * is three rows at three different depths, the fault is in what render.c
 * emits, not in how layout treats it. */
static void survey(struct node_handle h, float oy, int depth) {
    struct scene_node *n = scene_resolve(&sa, h);
    if (!n) return;
    float y = oy + n->ty;
    /* a "line box" = a group with text children */
    int words = 0;
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        if (cn->kind == SCENE_NODE_TEXT) words++;
        c = cn->next_sibling;
    }
    if (words > 1)
        printf("  row y=%-7.1f h=%-6.1f words=%-3d depth=%d\n", y, n->height, words, depth);
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(&sa, c);
        if (!cn) break;
        survey(c, y, depth + 1);
        c = cn->next_sibling;
    }
}

/* Assertions that must HOLD, not merely print. `make browser-render` is the
 * fast loop for this whole stack, and a loop that always exits 0 is a report,
 * not a test. */
static int g_fail;

int main(int argc, char **argv) {
    const char *doc = argc > 1 ? argv[1] : "system/web/index.html";
    int W = argc > 2 ? atoi(argv[2]) : 940;
    int H = argc > 3 ? atoi(argv[3]) : 620;
    int maxdepth = argc > 4 ? atoi(argv[4]) : 6;
    const char *png = argc > 5 ? argv[5] : 0;
    g_busy = (argc > 6 && argv[6][0] == 'b');

    size_t rl = 0, bl = 0, dl = 0;
    uint8_t *reg  = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", &rl);
    uint8_t *bold = read_file("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", &bl);
    uint8_t *src  = read_file(doc, &dl);
    if (!reg || !bold) { fprintf(stderr, "could not load DejaVu fonts\n"); return 1; }
    if (!src) { fprintf(stderr, "could not read %s\n", doc); return 1; }
    uint32_t fr = font_load(reg, rl), fb = font_load(bold, bl);
    font_install_backend();

    g_root = html_parse(&g_doc, (const char *)src, dl, g_nodes, NODE_MAX, g_strs, STR_MAX);
    css_sheet_parse(&g_sheet, g_doc.css, g_doc.css_len);
    g_doc_base = doc;
    printf("%s: %zu bytes -> %d nodes%s, root %d, %d css rule%s%s\n", doc, dl, g_doc.n,
           g_doc.truncated ? " (TRUNCATED)" : "", g_root, g_sheet.n,
           g_sheet.n == 1 ? "" : "s", g_sheet.truncated ? " (TRUNCATED)" : "");
    if (g_root < 0) return 1;

    scene_arena_init(&sa);
    layout_arena_init(&la);
    ui_theme_use_dark(true);
    ui_theme_set_fonts(fr, fb);
    ui_init(&sa, &la);

    em_set_viewport((float)W, (float)H);
    ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
    ui_run_layout((float)W, (float)H);

    printf("\n--- resolved tree (depth <= %d) ---\n", maxdepth);
    dump(ui_scene_of(ui_root()), 0, 0, 0, maxdepth);

    printf("\n--- line boxes (a wrapped paragraph should be ONE row) ---\n");
    survey(ui_scene_of(ui_root()), 0, 0);

    /* --- scroll cost: N build+layout passes, exactly what a wheel tick costs.
     * The scroll offset changes each pass so this measures the SCROLLING case,
     * not a fully-static frame. --- */
    {
        struct timespec t0, t1;
        int N = 60;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++) {
            g_scroll = (float)(i * 17 % 300);
            ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
            ui_run_layout((float)W, (float)H);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / N;
        printf("\n--- scroll cost: %.2f ms per build+layout pass (host) ---\n", ms);
    }

    /* --- scroll-blit correctness: an INCREMENTAL scroll frame must be pixel-
     * identical to a from-scratch render at the same offset. The blit is an
     * optimization; if it can be told apart from the real thing, it is a bug.
     * Exercises the renderer's Step 1s exactly as a wheel tick does. --- */
    {
        int Wp = W, Hp = H;
        size_t nbytes = (size_t)Wp * Hp * 4;
        uint32_t *pa = malloc(nbytes), *pb = malloc(nbytes);
        const struct ui_theme *t = ui_theme();
        uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                    | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
        for (int i = 0; i < Wp * Hp; i++) { pa[i] = bg; pb[i] = bg; }
        struct render_target ta = { pa, (uint32_t)Wp, (uint32_t)Hp, (uint32_t)Wp * 4, EMBK_PIXFMT_BGRA8888_PRE };
        struct render_target tb = { pb, (uint32_t)Wp, (uint32_t)Hp, (uint32_t)Wp * 4, EMBK_PIXFMT_BGRA8888_PRE };

        struct scene_renderer ra; scene_render_init(&ra, cpu_backend_get());
        /* frame 1 at scroll 0, then frame 2 at scroll 64: the INCREMENTAL path */
        g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)Wp, (float)Hp);
        scene_render_frame(&ra, &sa, ui_scene_of(ui_root()), &ta);
        g_scroll = 64;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)Wp, (float)Hp);
        scene_render_frame(&ra, &sa, ui_scene_of(ui_root()), &ta);
        int blitted = ra.has_scroll_present;

        /* fresh renderer straight to scroll 64: the REFERENCE */
        struct scene_renderer rb; scene_render_init(&rb, cpu_backend_get());
        scene_render_frame(&rb, &sa, ui_scene_of(ui_root()), &tb);

        int diff = 0, dy0 = -1, dy1 = -1;
        for (int i = 0; i < Wp * Hp; i++) if (pa[i] != pb[i]) {
            diff++;
            int y = i / Wp;
            if (dy0 < 0) dy0 = y;
            dy1 = y;
        }
        if (diff) printf("    (differing rows: %d..%d)\n", dy0, dy1);
        printf("--- scroll blit: %s (blit path %s, %d differing px) ---\n",
               diff == 0 ? "PIXEL-EXACT" : "MISMATCH", blitted ? "TAKEN" : "not taken", diff);
        /* A check that passes because the code under test never ran is not a
         * check. This one read PIXEL-EXACT for as long as the classifier was
         * vetoing every real page -- the two renders agreed because they were
         * the same render. Not being taken is now a FAILURE. */
        if (!blitted || diff) {
            printf("*** scroll-blit check FAILED: %s ***\n",
                   !blitted ? "the blit path was never exercised" : "pixels differ");
            g_fail++;
        }
        scene_render_destroy(&ra); scene_render_destroy(&rb);
        free(pa); free(pb);
    }

    /* --- THE REFLOW CHECK ------------------------------------------------
     * The claim images make is not "they render" -- it is that the page does
     * not MOVE when they land. So lay the document out twice, once with every
     * picture still outstanding and once with them all decoded, and compare
     * where the text ended up. Any difference is the reader's paragraph
     * jumping out from under them. --- */
    {
        float y_before = 0, y_after = 0;
        int   n_before = 0, n_after = 0;

        HDEFER = 1; imgcache_reset(); g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        text_extent(ui_scene_of(ui_root()), 0, 0, &y_before, &n_before);

        HDEFER = 0; imgcache_reset(); g_scroll = 0;
        ui_frame_begin(); em_new_frame(); app(); em_flush(); ui_frame_end();
        ui_run_layout((float)W, (float)H);
        text_extent(ui_scene_of(ui_root()), 0, 0, &y_after, &n_after);

        /* The verdict is Y, not the node count: an image with NO stated size
         * legitimately shows alt text until it arrives, so the counts differ
         * by design. What must not change is where the text sits. */
        printf("--- reflow: last text y=%.1f before images -> y=%.1f after : %s"
               "   (text nodes %d -> %d)%s ---\n",
               y_before, y_after,
               y_before == y_after ? "NO REFLOW" : "PAGE MOVED",
               n_before, n_after,
               n_before != n_after ? "  [unsized images fell back to alt text]" : "");
        /* Asserted only when the counts match. An image with NO stated size
         * cannot reserve its box, so it shows alt text and then reflows when
         * the real thing lands -- true here and true in every other browser,
         * which is what width/height are for. A differing count is exactly
         * that case, and failing on it would be asserting something the
         * browser never claimed. */
        if (n_before != n_after) {
            printf("    (reflow NOT asserted: an unsized image fell back to alt text)\n");
        } else if (y_before != y_after) {
            printf("*** reflow check FAILED: the page moved when images arrived ***\n");
            g_fail++;
        }
    }

    if (png) {
        struct render_target rt;
        rt.pixels = malloc((size_t)W * H * 4); rt.width = W; rt.height = H;
        rt.stride = W * 4; rt.format = EMBK_PIXFMT_BGRA8888_PRE;
        const struct ui_theme *t = ui_theme();
        uint8_t br = (uint8_t)(t->bg.b * 255), bg = (uint8_t)(t->bg.g * 255),
                rr = (uint8_t)(t->bg.r * 255);
        for (int i = 0; i < W * H; i++)
            ((uint32_t *)rt.pixels)[i] = (255u << 24) | ((uint32_t)rr << 16) |
                                         ((uint32_t)bg << 8) | br;
        struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
        FILE *f = fopen(png, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            uint32_t px = ((uint32_t *)rt.pixels)[i];
            uint8_t rgb[3] = { (uint8_t)((px >> 16) & 255), (uint8_t)((px >> 8) & 255),
                               (uint8_t)(px & 255) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        fprintf(stderr, "wrote %s (%dx%d)\n", png, W, H);
    }
    if (g_fail) fprintf(stderr, "\n*** browser-render: %d CHECK(S) FAILED ***\n", g_fail);
    return g_fail ? 1 : 0;
}

/* --- imgcache, host flavour --------------------------------------------- *
 * The on-target cache fetches over the network on a worker thread; here the
 * pictures are simply files. Same interface, so render.c is identical in both
 * worlds -- which is what makes the host render worth trusting. */
static struct img_slot H[IMG_SLOTS];
static uint32_t HARENA[IMG_MAX_PX];
static size_t   HUSED;
static uint8_t  HSCR[IMG_MAX_PX * 4 + IMG_MAX_DIM + 64];

void imgcache_reset(void) { memset(H, 0, sizeof H); HUSED = 0; }
int  imgcache_pump(void) { return 0; }
int  imgcache_pending(void) { return 0; }

struct img_slot *imgcache_want(const char *url) {
    if (!url || !url[0]) return 0;
    if (HDEFER) {
        for (int i = 0; i < IMG_SLOTS; i++)
            if (H[i].state != IMG_EMPTY && !strcmp(H[i].url, url)) return &H[i];
        for (int i = 0; i < IMG_SLOTS; i++) {
            if (H[i].state != IMG_EMPTY) continue;
            snprintf(H[i].url, sizeof H[i].url, "%s", url);
            H[i].state = IMG_WANTED;
            return &H[i];
        }
        return 0;
    }
    for (int i = 0; i < IMG_SLOTS; i++)
        if (H[i].state != IMG_EMPTY && !strcmp(H[i].url, url)) return &H[i];
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (H[i].state != IMG_EMPTY) continue;
        snprintf(H[i].url, sizeof H[i].url, "%s", url);
        size_t n = 0;
        const char *path = url;
        while (*path == '/') path++;              /* repo-relative on the host */
        uint8_t *bytes = read_file(path, &n);
        if (!bytes) { H[i].state = IMG_FAILED; return &H[i]; }
        uint32_t w = 0, h = 0;
        int is_jpeg = n > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8;
        int probed = is_jpeg ? jpeg_probe(bytes, n, &w, &h) : png_probe(bytes, n, &w, &h);
        if (probed != 0 ||
            w > IMG_MAX_DIM || h > IMG_MAX_DIM || (size_t)w * h > IMG_MAX_PX) {
            H[i].state = IMG_FAILED; free(bytes); return &H[i];
        }
        uint32_t *dst = HARENA + HUSED;
        int drc = is_jpeg
            ? jpeg_decode(bytes, n, dst, (size_t)w * h * 4, HSCR, sizeof HSCR, &w, &h)
            : png_decode(bytes, n, dst, (size_t)w * h * 4, HSCR, sizeof HSCR, &w, &h);
        if (drc != 0) {
            H[i].state = IMG_FAILED; free(bytes); return &H[i];
        }
        HUSED += (size_t)w * h;
        H[i].px = dst; H[i].w = w; H[i].h = h; H[i].state = IMG_READY;
        free(bytes);
        return &H[i];
    }
    return 0;
}
