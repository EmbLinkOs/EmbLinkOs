/* render_test.c -- Terminal's renderer (user/apps/term/render.c, glyphs.c)
 * on the host, with the faces the image ships: MesloLGS Nerd Font Mono from
 * the tree, DejaVu from the host's fonts as the fallback (skipped, and said
 * so, when the host has none). Screens are fed through the real screen model
 * and the pixels checked.
 *
 *     render_test [out.ppm]     -- also writes a sample screen to look at */
#include "render.h"
#include "glyphs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}
static struct font *load(const char *path) {
    size_t n; uint8_t *d = slurp(path, &n);
    return d ? font_for_handle(font_load(d, n)) : NULL;
}
static struct font *load_dejavu(const char *name) {
    const char *dirs[] = { getenv("EMBK_FONT_DIR"), "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu",
                           "/usr/share/fonts/TTF", NULL, "/Library/Fonts" };
    char home[512];
    snprintf(home, sizeof home, "%s/Library/Fonts", getenv("HOME") ? getenv("HOME") : "");
    dirs[4] = home;
    for (int i = 0; i < 6; i++) {
        if (!dirs[i]) continue;
        char path[640]; snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        struct font *f = load(path);
        if (f) return f;
    }
    return NULL;
}

static struct vt v;
static struct term_render r;
static struct term_frame fr = { .blink_on = true };
static void feed(const char *s) { vt_feed(&v, s, strlen(s)); }
static uint32_t px(int x, int y) { return r.pixels[(size_t)y * r.width + x] & 0xffffff; }
static int ink(int x0, int y0, int x1, int y1, uint32_t bg) {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += px(x, y) != bg;
    return n;
}
static uint32_t BG, FG;
static void start(int cols, int rows) {
    vt_free(&v);
    vt_init(&v, cols, rows, NULL, NULL);
    term_render_resize(&r, cols, rows);
    fr = (struct term_frame){ .blink_on = true };
    BG = vt_rgb(&v, VT_COLOR_DEFAULT, false); FG = vt_rgb(&v, VT_COLOR_DEFAULT, true);
}
static void frame(void) { term_render_frame(&r, &v, &fr); }
static int cx0(int col) { return col * r.cell_w; }
static int cy0(int row) { return row * r.cell_h; }

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", r.width, r.height);
    for (int i = 0; i < r.width * r.height; i++) {
        uint32_t p = r.pixels[i];
        fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
    }
    fclose(f);
    printf("  wrote %s (%d x %d)\n", path, r.width, r.height);
}

int main(int argc, char **argv) {
    printf("=== Terminal renderer ===\n");
    const char *m = "third_party/fonts/meslo-nf/MesloLGSNerdFontMono-";
    char path[256];
    struct font *faces[4];
    static const char *const names[4] = { "Regular", "Bold", "Italic", "BoldItalic" };
    for (int i = 0; i < 4; i++) { snprintf(path, sizeof path, "%s%s.ttf", m, names[i]); faces[i] = load(path); }
    CHECK(faces[0] && faces[1] && faces[2] && faces[3], "the four Meslo faces load from the tree");
    if (!faces[0]) return 1;
    struct font *fallback[2] = { load_dejavu("DejaVuSansMono.ttf"), load_dejavu("DejaVuSans.ttf") };
    int nfallback = fallback[0] && fallback[1] ? 2 : 0;
    if (!nfallback) printf("  (no DejaVu on this host: the fallback checks are skipped)\n");
    CHECK(term_render_fonts(&r, faces, fallback, nfallback, 14), "fonts at 14 px");
    CHECK(r.cell_w == 8 && r.cell_h == 18 && r.baseline == 14, "Meslo at 14 px: 8 x 18 cells, baseline 14 (as Terminal.app's grid)");

    printf("box drawing joins cell to cell:\n");
    start(20, 8);
    feed("┌──┬──┐\r\n│  │  │\r\n├──┼──┤\r\n└──┴──┘");
    frame();
    int hy = cy0(0) + (r.cell_h - r.line) / 2, vx = cx0(0) + (r.cell_w - r.line) / 2;
    int gaps = 0;
    for (int x = vx; x < cx0(6) + r.cell_w / 2; x++) gaps += px(x, hy) != FG;
    CHECK(gaps == 0, "the top line runs unbroken from the first corner to the last");
    gaps = 0;
    for (int y = hy; y < cy0(3) + r.cell_h / 2; y++) gaps += px(vx, y) != FG;
    CHECK(gaps == 0, "the left line runs unbroken down four rows");
    CHECK(px(cx0(0), hy) == BG && px(vx, cy0(0)) == BG, "the corner does not run past itself");
    gaps = 0;
    for (int y = 0; y < cy0(4); y++) gaps += px(cx0(3) + (r.cell_w - r.line) / 2, y) != FG && y >= hy && y < cy0(3) + r.cell_h / 2;
    CHECK(gaps == 0, "the middle line meets the tees and the cross");
    start(20, 4);
    feed("╔═╦═╗\r\n║ ║ ║\r\n╚═╩═╝");
    frame();
    int l = r.line, outer = cy0(0) + (r.cell_h - 3 * l) / 2, ox = cx0(0) + (r.cell_w - 3 * l) / 2;
    gaps = 0;
    for (int x = ox; x < cx0(4) + r.cell_w / 2; x++) gaps += px(x, outer) != FG;
    CHECK(gaps == 0, "a double frame's outer line runs unbroken");
    gaps = 0;
    for (int y = outer; y < cy0(2) + r.cell_h / 2; y++) gaps += px(ox, y) != FG;
    CHECK(gaps == 0, "and down its side");
    CHECK(px(ox + 2 * l, outer + 2 * l) == FG && px(ox + 2 * l - 1, outer + 2 * l - 1) == BG,
          "the inner line turns its own corner, inside the outer one");

    printf("Powerline, blocks, braille fill the cell:\n");
    start(20, 4);
    feed("\033[44m A \033[34;42m\ue0b0\033[30m B \033[m\r\n█▀⣿░");
    frame();
    uint32_t blue = vt_rgb(&v, VT_COLOR_INDEX(4), true), green = vt_rgb(&v, VT_COLOR_INDEX(2), true);
    int seam = 0;
    for (int y = 1; y < r.cell_h - 1; y++) seam += px(cx0(3), y) != blue;     /* the two tips are antialiased */
    CHECK(seam == 0, "the separator's flat side covers the cell's height (no seam)");
    uint32_t tip = px(cx0(4) - 1, r.cell_h / 2);
    CHECK((tip & 0xff) > (green & 0xff) && px(cx0(4) - 1, 0) == green && px(cx0(4), r.cell_h / 2) == green,
          "its point reaches the cell's edge, the next segment's colour around it");
    CHECK(ink(cx0(0), cy0(1), cx0(1), cy0(2), BG) == r.cell_w * r.cell_h, "a full block fills every pixel");
    CHECK(px(cx0(1), cy0(1)) == FG && px(cx0(1), cy0(2) - 1) == BG, "the upper half: top filled, bottom not");
    int dots = 0;
    for (int i = 0; i < 8; i++) {
        static const int col[8] = { 0, 0, 0, 1, 1, 1, 0, 1 }, row[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
        dots += px(cx0(2) + (col[i] ? r.cell_w * 3 / 4 : r.cell_w / 4), cy0(1) + r.cell_h * (2 * row[i] + 1) / 8) != BG;
    }
    CHECK(dots == 8, "all eight braille dots");
    uint32_t shade = px(cx0(3), cy0(1));
    CHECK(shade != BG && shade != FG, "a light shade is between ground and ink");

    printf("faces, colours, fallback:\n");
    start(40, 6);
    feed("Bold\r\n\033[1mBold\033[m\r\n\033[3mI\033[m I\r\n\033[1;31mX\033[m\033[31mX");
    frame();
    int regular_ink = ink(0, cy0(0), cx0(4), cy0(1), BG), bold_ink = ink(0, cy0(1), cx0(4), cy0(2), BG);
    CHECK(bold_ink > regular_ink * 11 / 10, "bold is drawn from the bold face (more ink)");
    /* italic: the stem's top is right of its foot; upright, above it */
    int lean[2];
    for (int k = 0; k < 2; k++) {
        int top_x = 0, bottom_x = 0, nt = 0, nb = 0, x0 = cx0(2 * k);
        for (int x = x0; x < x0 + r.cell_w; x++) {
            if (px(x, cy0(2) + r.baseline - 9) != BG) { top_x += 16 * x; nt++; }
            if (px(x, cy0(2) + r.baseline - 3) != BG) { bottom_x += 16 * x; nb++; }
        }
        lean[k] = nt && nb ? top_x / nt - bottom_x / nb : -999;
    }
    CHECK(lean[0] >= 8 && lean[1] > -4 && lean[1] < 4, "italic leans: its stem's top is right of its foot, upright's is not");
    uint32_t c1 = vt_rgb(&v, VT_COLOR_INDEX(1), true), c9 = vt_rgb(&v, VT_COLOR_INDEX(9), true);
    int bright = 0, plain = 0;
    for (int y = cy0(3); y < cy0(4); y++) for (int x = 0; x < cx0(1); x++) bright += px(x, y) == c9;
    for (int y = cy0(3); y < cy0(4); y++) for (int x = cx0(1); x < cx0(2); x++) plain += px(x, y) == c1;
    CHECK(!bright && plain, "bold red is red by default");
    fr.bold_is_bright = true; term_render_invalidate(&r); frame();
    bright = 0;
    for (int y = cy0(3); y < cy0(4); y++) for (int x = 0; x < cx0(1); x++) bright += px(x, y) == c9;
    CHECK(bright > 0, "with bold-as-bright, bold red is bright red");
    start(20, 3);
    if (nfallback) {
        uint32_t candidates[] = { 0x0531, 0x10A0, 0x2C00, 0x05D0, 0x0E01 }, pick = 0;
        for (unsigned i = 0; i < sizeof candidates / sizeof *candidates && !pick; i++)
            if (!font_has_glyph(faces[0], candidates[i]) && term_render_font_for(&r, candidates[i], 0)) pick = candidates[i];
        CHECK(pick != 0, "a letter Meslo lacks is found in DejaVu");
        char text[8]; unsigned n = vt_encode(pick, text); text[n] = 0;
        feed(text); frame();
        CHECK(ink(cx0(0), 0, cx0(1), r.cell_h, BG) > 6, "and drawn from it, in the cell");
    }
    feed("\r\n\xe7\x95\x8c");                     /* U+754C: wide, in no face here */
    frame();
    int box_t = 0, box_b = 0;
    for (int x = cx0(0) + 2; x < cx0(2) - 2; x++) { box_t += px(x, cy0(1) + 2) == FG; box_b += px(x, cy0(2) - 3) == FG; }
    CHECK(term_render_font_for(&r, 0x754C, 0) == NULL && box_t == 2 * r.cell_w - 4 && box_b == 2 * r.cell_w - 4,
          "a character no face has is a box, two cells wide for a wide one");
    start(10, 2);
    feed("e\r\ne\xcc\x81");
    frame();
    int plain_top = ink(0, 0, r.cell_w, r.baseline - 9, BG), acute_top = ink(0, cy0(1), r.cell_w, cy0(1) + r.baseline - 9, BG);
    CHECK(plain_top == 0 && acute_top > 2, "a combining accent is drawn over its letter");

    printf("cursors:\n");
    start(10, 2);
    feed("ab");
    uint32_t cur = vt_cursor_rgb(&v);
    fr.cursor = true; fr.cursor_x = 0; fr.cursor_y = 0; fr.cursor_shape = 2;
    frame();
    CHECK(px(cx0(0), 0) == cur && px(cx0(0) + r.cell_w - 1, r.cell_h - 1) == cur, "a block cursor fills its cell");
    int on_letter = 0;
    for (int y = 0; y < r.cell_h; y++) for (int x = 0; x < r.cell_w; x++) on_letter += px(x, y) == BG;
    CHECK(on_letter > 3, "and the letter under it shows in the ground colour");
    fr.cursor_shape = 6; frame();
    CHECK(px(0, r.cell_h / 2) == cur && px(r.cell_w - 1, 2) != cur, "a bar cursor is a bar");
    fr.cursor_shape = 4; frame();
    CHECK(px(r.cell_w / 2, r.cell_h - 1) == cur && px(r.cell_w / 2, 1) != cur, "an underline cursor is a line under");
    fr.cursor_shape = 2; fr.cursor_hollow = true; frame();
    CHECK(px(0, 0) == cur && px(r.cell_w / 2, r.cell_h / 2) != cur, "without the keyboard, an outline");

    printf("only what changed repaints:\n");
    start(20, 6);
    fr.cursor = false;
    feed("one\r\ntwo\r\nthree");
    frame();
    CHECK(!term_render_frame(&r, &v, &fr), "nothing changed: nothing repainted");
    feed("\033[2;1HTWO");
    CHECK(term_render_frame(&r, &v, &fr) && r.dmg_y0 == cy0(1) && r.dmg_y1 == cy0(2), "one row changed: that row's pixels");
    feed("\033[5;1H\033[5mblink\033[m");
    frame();
    fr.blink_on = false;
    CHECK(term_render_frame(&r, &v, &fr) && r.dmg_y0 == cy0(4) && r.dmg_y1 == cy0(5),
          "the blink phase repaints the blinking row alone");
    CHECK(ink(0, cy0(4), r.width, cy0(5), BG) == 0, "and its text is off in this phase");
    fr.blink_on = true;
    struct vt_palette pal = v.base; pal.fg = 0x123456; vt_set_palette(&v, &pal);
    CHECK(term_render_frame(&r, &v, &fr) && r.dmg_y0 == 0 && r.dmg_y1 >= cy0(5), "a palette change repaints the text it colours");
    /* the same screen painted afresh is the same pixels as painted row by row */
    feed("\033[3;1H\033[1;4:3;58:5:196mcurly\033[m \033[4:4mdot\033[m \033[4:5mdash\033[9m x\033[m");
    fr.cursor = true; fr.cursor_x = 2; fr.cursor_y = 2;
    frame();
    size_t bytes = (size_t)r.width * r.height * sizeof *r.pixels;
    uint32_t *incremental = malloc(bytes);
    memcpy(incremental, r.pixels, bytes);
    term_render_invalidate(&r); frame();
    CHECK(!memcmp(incremental, r.pixels, bytes), "painting only what changed gives the same pixels as painting everything");
    free(incremental);
    /* scrolling: last frame's rows move, only the new row is drawn, and the
     * result is the same pixels as drawing it all */
    start(30, 8);
    fr.cursor = true;
    for (int i = 0; i < 6; i++) { char line[48]; snprintf(line, sizeof line, "\033[3%dmline %d \u2500\u2500 \ue0b0\033[m\r\n", i % 8, i); feed(line); }
    frame();
    int scrolled_ok = 1, painted_max = 0;
    uint32_t *fresh = malloc((size_t)r.width * r.height * sizeof *r.pixels);
    for (int i = 6; i < 20; i++) {
        char line[48]; snprintf(line, sizeof line, "\033[1;3%dmline %d\033[m\r\n", i % 8, i); feed(line);
        fr.cursor_x = v.primary.cursor.x; fr.cursor_y = v.primary.cursor.y;
        frame();
        if (r.rows_painted > painted_max) painted_max = r.rows_painted;
        if (i >= 8 && r.rows_moved < r.rows - 2) scrolled_ok = 0;
        memcpy(fresh, r.pixels, (size_t)r.width * r.height * sizeof *r.pixels);
        term_render_invalidate(&r); frame();
        if (memcmp(fresh, r.pixels, (size_t)r.width * r.height * sizeof *r.pixels)) scrolled_ok = 0;
    }
    free(fresh);
    CHECK(scrolled_ok, "a scroll moves the painted rows and gives the same pixels as painting everything");
    CHECK(painted_max <= 3, "a line scrolling in draws the new rows and the cursor's, not the screen");
    start(20, 6);
    feed("\033[3;1H\033[1;4:3;58:5:196mcurly\033[m");
    frame();
    uint32_t red = vt_rgb(&v, VT_COLOR_INDEX(196), true);
    int curly = 0;
    for (int y = cy0(2); y < cy0(3); y++) for (int x = 0; x < cx0(5); x++) curly += px(x, y) == red;
    CHECK(curly > 2 * r.cell_w, "the curly underline in its own colour");

    printf("marks: selection, matches, link:\n");
    start(30, 6);
    feed("one needle\r\ntwo\r\nthree needle\r\nsee https://a.invalid/x\r\n");
    struct term_selection sel = {0};
    struct term_point p0 = { vt_row_id(&v, 1, 0), 0 }, p1 = { vt_row_id(&v, 1, 0), 3 };
    term_select_start(&sel, &v, p0, TERM_SELECT_CHARS);
    term_select_extend(&sel, &v, p1);
    fr = (struct term_frame){ .blink_on = true, .selection = &sel, .selection_bg = 0x335577, .find = "needle",
                              .find_bg = 0xaa8800, .find_current_bg = 0xff6600, .find_fg = 0x101010 };
    term_render_invalidate(&r); frame();
    CHECK(px(cx0(0), cy0(1)) == 0x335577 && px(cx0(3), cy0(1)) == BG, "the selection's cells take its colour, and stop where it stops");
    CHECK(px(cx0(4), cy0(0)) == 0xaa8800 && px(cx0(4), cy0(2) + 1) == BG && px(cx0(6), cy0(2)) == 0xaa8800,
          "every match is marked");
    struct term_match found;
    struct term_point bottom = { vt_row_id(&v, 5, 0), 30 };
    CHECK(term_find(&v, "needle", bottom, true, &found), "the last match found");
    fr.find_current = found; fr.find_current_set = true;
    CHECK(term_render_frame(&r, &v, &fr) && r.rows_painted == 1 && px(cx0(6), cy0(2)) == 0xff6600 && px(cx0(4), cy0(0)) == 0xaa8800,
          "the current one is stronger, and only its row repaints");
    struct term_match link;
    char url[64];
    struct term_point on = { vt_row_id(&v, 3, 0), 8 };
    CHECK(term_link_at(&v, on, url, sizeof url, &link), "a link under the pointer");
    fr.link = link; fr.link_set = true;
    frame();
    int under = 0;
    for (int x = cx0(4); x < cx0(23); x++) under += px(x, cy0(3) + r.underline) == FG;
    CHECK(r.rows_painted == 1 && under == cx0(23) - cx0(4), "it is underlined, its row alone repainted");
    p1.row = vt_row_id(&v, 2, 0); p1.col = 5;
    term_select_extend(&sel, &v, p1);
    frame();
    CHECK(r.rows_painted == 2 && px(cx0(10), cy0(1)) == 0x335577, "a selection that grows repaints the rows it changes");
    term_select_clear(&sel);
    frame();
    CHECK(r.rows_painted == 2 && px(cx0(0), cy0(1)) == BG, "and cleared, those rows again");

    printf("procedural glyphs at other sizes:\n");
    uint8_t mask[64 * 128];
    int ok = 1;
    for (int w = 5; w <= 32; w += 3) for (int h = 9; h <= 64; h += 5)
        for (uint32_t cp = 0x2500; cp <= 0x259f; cp++) if (!term_glyph_draw(cp, mask, w, h, 1 + w / 12)) ok = 0;
    CHECK(ok, "every box and block character draws at every size");
    term_glyph_draw(0x2500, mask, 9, 21, 1);
    int left = 0, right = 0;
    for (int y = 0; y < 21; y++) { left += mask[y * 9] == 255; right += mask[y * 9 + 8] == 255; }
    CHECK(left == 1 && right == 1, "a light line is one pixel thick and reaches both edges");

    /* a busy screen, once from scratch: the cost of a full repaint */
    start(120, 30);
    for (int i = 0; i < 30; i++) {
        char line[200];
        snprintf(line, sizeof line, "\033[3%dm%03d\033[m the quick brown fox \033[1mjumps\033[m over │ ┌─┐ \ue0b0 \033[4mthe lazy dog\033[m %d\r\n", i % 8, i, i * 7);
        feed(line);
    }
    clock_t t0 = clock();
    for (int i = 0; i < 10; i++) { term_render_invalidate(&r); frame(); }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 10;
    printf("  full repaint of 120 x 30: %.2f ms on this host\n", ms);
    CHECK(ms < 100, "a full repaint stays well inside a frame");

    printf("a profile's look: bold colour, a translucent ground, spacing:\n");
    start(10, 3);
    feed("\033[1mB\033[m \033[1;31mR\033[m");
    fr.bold_fg_set = true; fr.bold_fg = 0x00ff00;
    frame();
    int greenish = 0, reddish = 0;
    for (int y = cy0(0); y < cy0(1); y++) for (int x = cx0(0); x < cx0(1); x++) greenish += px(x, y) == 0x00ff00;
    for (int y = cy0(0); y < cy0(1); y++) for (int x = cx0(2); x < cx0(3); x++) reddish += px(x, y) == vt_rgb(&v, VT_COLOR_INDEX(1), true);
    CHECK(greenish > 10 && reddish > 10, "bold in the default colour takes the bold colour; a coloured bold keeps its own");
    start(10, 3);
    feed("a \033[41m \033[m\033[7m \033[m");
    fr.ground_transparency = 128;
    frame();
    uint32_t ground = r.pixels[(size_t)cy0(0) * r.width + cx0(5)];
    unsigned ga = ground >> 24;
    CHECK(ga == 127 && ((ground >> 16) & 255) <= ga && ((ground >> 8) & 255) <= ga && (ground & 255) <= ga,
          "the ground shows through: alpha 127, its colour premultiplied");
    CHECK(r.pixels[(size_t)cy0(0) * r.width + cx0(2)] >> 24 == 255 && r.pixels[(size_t)cy0(0) * r.width + cx0(3)] >> 24 == 255,
          "a coloured cell and an inverse one stay opaque");
    unsigned most = 0;
    for (int y = cy0(0); y < cy0(1); y++) for (int x = cx0(0); x < cx0(1); x++) {
        unsigned a = r.pixels[(size_t)y * r.width + x] >> 24;
        if (a > most) most = a;
    }
    CHECK(most == 255, "text over the ground is opaque where it covers");
    struct term_selection all = {0};
    term_select_all(&all, &v);
    fr.selection = &all; fr.selection_bg = 0x335577;
    frame();
    CHECK(r.pixels[(size_t)cy0(0) * r.width + cx0(5)] >> 24 == 255, "a selected cell is opaque");
    fr.selection = NULL;
    term_render_spacing(&r, 1.5f, 1.25f);
    term_render_fonts(&r, faces, fallback, nfallback, 14);
    CHECK(r.cell_w == 10 && r.cell_h == 27 && r.glyph_dx == 1, "line spacing 1.5 and letter spacing 1.25: 10 x 27 cells");
    start(4, 2);
    feed("M");
    frame();
    int minx = r.cell_w, maxx = -1;
    for (int y = 0; y < r.cell_h; y++) for (int x = 0; x < r.cell_w; x++)
        if (px(x, y) != BG) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
    int margin_l = minx, margin_r = r.cell_w - 1 - maxx;
    CHECK(maxx >= 0 && abs(margin_l - margin_r) <= 1, "the glyph sits in the middle of the wider cell");
    term_render_spacing(&r, 1, 1);
    term_render_fonts(&r, faces, fallback, nfallback, 14);
    CHECK(r.cell_w == 8 && r.cell_h == 18 && r.glyph_dx == 0, "and back to the font's own cell");

    printf("the cells inside margins:\n");
    start(4, 2);
    term_render_layout(&r, 4, 2, 4 * r.cell_w + 23, 2 * r.cell_h + 31, 10, 12);
    fr.ground_transparency = 51;
    feed("\033[41mX");
    frame();
    CHECK(r.width == 4 * r.cell_w + 23 && r.height == 2 * r.cell_h + 31 && r.dmg_y0 == 0 && r.dmg_y1 == r.height,
          "the picture is the size asked, and the first frame is all of it");
    uint32_t corner = r.pixels[0], edge_px = r.pixels[(size_t)20 * r.width + r.width - 1];
    CHECK(corner >> 24 == 204 && corner == edge_px, "the margins are the ground, translucent too");
    CHECK(r.pixels[(size_t)12 * r.width + 10] == (0xff000000u | vt_rgb(&v, VT_COLOR_INDEX(1), false)) &&
          r.pixels[(size_t)11 * r.width + 10] == corner && r.pixels[(size_t)12 * r.width + 9] == corner,
          "the first cell starts at (10, 12)");
    feed("\r\nY");
    frame();
    CHECK(r.dmg_y0 == 12 + r.cell_h && r.dmg_y1 == 12 + 2 * r.cell_h, "a changed row is damaged where it is drawn");

    if (argc > 1) {
        start(84, 16);
        fr = (struct term_frame){ .blink_on = true, .cursor = true, .cursor_shape = 2, .bold_is_bright = false };
        feed("\033[1mMesloLGS Nerd Font Mono\033[m, \033[3mitalic\033[m, \033[1;3mbold italic\033[m; DejaVu: \xd4\xb1\xd5\xa2 \xce\xa9\r\n");
        feed("╭─ \033[44;30m ~/src/emblink \033[34;42m\ue0b0\033[30m \ue0a0 master \033[32;49m\ue0b0\033[m\r\n");
        feed("╰─ \033[32m❯\033[m ls\r\n");
        feed("┌──┬──┐ ╔══╦══╗ ╭──╮ ▁▂▃▄▅▆▇█ ░▒▓ ⣿⡇⢀ \ue0b2\ue0b4\ue0b6\ue0b8\ue0ba\ue0bc\ue0be\r\n");
        feed("│  │  │ ║  ║  ║ │  │ ▙▛▜▟ ╱╲╳ \ue0b1\ue0b3\ue0b5\ue0b7\r\n");
        feed("├──┼──┤ ╠══╬══╣ ╰──╯ ┄┄┈╌ ┆┊╎\r\n");
        feed("└──┴──┘ ╚══╩══╝ ╒╕╘╛ ╓╖╙╜ ╞╡╟╢ ╤╧╥╨╪╫\r\n");
        feed("\033[4msingle\033[m \033[21mdouble\033[m \033[4:3;58:2::255:95:95mcurly\033[m \033[4:4mdotted\033[m \033[4:5mdashed\033[m \033[9mstrike\033[m \033[53moverline\033[m caf\xc3\xa9 e\xcc\x81 \xe7\x95\x8c\r\n");
        feed("\uf113 \ue718 \U000f0001 \U000f0a1a \uf015 \uf07b \uf489 \ue7a8 \uf120\r\n");
        for (int i = 0; i < 16; i++) { char s[32]; snprintf(s, sizeof s, "\033[48;5;%dm  ", i); feed(s); }
        feed("\033[m\r\n");
        for (int i = 0; i < 64; i++) { char s[48]; snprintf(s, sizeof s, "\033[48;2;%d;%d;%dm ", i * 4, 128, 255 - i * 4); feed(s); }
        feed("\033[m\r\n$ ");
        fr.cursor_x = v.primary.cursor.x; fr.cursor_y = v.primary.cursor.y;
        term_render_invalidate(&r); frame();
        write_ppm(argv[1]);
    }
    vt_free(&v);
    term_render_free(&r);
    printf("=== render-test: %s (%u checks, %u failures) ===\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
