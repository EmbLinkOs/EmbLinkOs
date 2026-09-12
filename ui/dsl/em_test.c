/* ui/dsl/em_test.c -- the MULTI-LINE editor's caret, selection and clipboard,
 * on the host.  make em-test  -> exits 0 iff every claim holds.
 *
 * WHY THIS FILE EXISTS AT ALL. ui/kit/kit_test.c covers the single-line field,
 * and user/note/edit_test.c covers the Note app's own buffer -- but
 * em_text_editor, which is the editor every EmUI application actually uses,
 * had no host test of any kind. It was only ever exercised by booting a
 * machine and typing, which is slow enough that in practice it was not
 * exercised at all.
 *
 * The bug that made the case: the day the keyboard driver began sending
 * editing commands as bytes 0xF8..0xFF, this editor's default branch inserted
 * ANY byte >= 0x80 as text -- correct for é and €, catastrophic for these --
 * so GUI+C in Note++ would have typed a garbage character into the document
 * instead of copying. The single-line field's host test caught its half of
 * that in a second. This is the other half. */
#include "ui.h"
#include "scene.h"
#include "layout.h"
#include "kit.h"
#include "theme.h"
#include "em.h"
#include "font.h"
#include "testfont.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
                              else { printf("  ok:   %s\n", (msg)); } } while (0)

static struct scene_arena  SA;
static struct layout_arena LA;

/* A clipboard of our own -- the kit takes one as two function pointers exactly
 * so that copy and cut can be tested here instead of only in QEMU. */
static char g_clip[512];
static unsigned g_clip_n;
static int clip_set(const char *b, unsigned n) {
    if (n > sizeof g_clip) n = sizeof g_clip;
    memcpy(g_clip, b, n); g_clip_n = n; return 0;
}
static int clip_get(char *b, unsigned cap) {
    unsigned n = g_clip_n < cap ? g_clip_n : cap;
    memcpy(b, g_clip, n); return (int)n;
}
static const char *clip_text(void) {
    static char out[513];
    memcpy(out, g_clip, g_clip_n); out[g_clip_n] = 0; return out;
}

static char DOC[512];
static int  CUR;

/* A clock the test winds by hand -- double-click is a real elapsed time, and
 * sleeping in a test is how a test becomes flaky on a loaded machine. */
static uint64_t g_ms = 1000;
static uint64_t fake_clock(void) { return g_ms; }

static void view(void) { em_text_editor(DOC, sizeof DOC, &CUR, 200); }

static void frame(const char *keys) {
    for (const char *k = keys; k && *k; k++) ui_input_char((unsigned char)*k);
    ui_frame_begin(); view(); ui_frame_end();
    ui_run_layout(400, 300);
}

/* The editor takes focus from a CLICK, so the test has to click it -- there is
 * no autofocus on an editor. A press and a release in two frames, because a
 * click is an edge and this pipeline reports edges. */
static void click_in(void) {
    ui_pointer(100, 100, true);
    frame(NULL);
    ui_pointer(100, 100, false);
    frame(NULL);
}

/* Press at (x,y), then release there: a click that PLACES the caret.
 *
 * IT ADVANCES THE CLOCK FIRST, so that consecutive gestures in this file are
 * separate ones. Without that, every click in the test is inside the
 * double-click window of the one before it and the run climbs forever -- which
 * silently disabled the drags, because a drag deliberately does not move the
 * caret while a multi-click owns the selection. Tests that want a real
 * double-click use click_fast(). */
static void click_fast(float x, float y) {
    ui_pointer(x, y, true);
    frame(NULL);
    ui_pointer(x, y, false);
    frame(NULL);
}
static void click_at(float x, float y) { g_ms += 900; click_fast(x, y); }

/* Press at one point, drag to another, release: a selection. */
static void drag(float x0, float y0, float x1, float y1) {
    g_ms += 900;
    ui_pointer(x0, y0, true);
    frame(NULL);
    ui_pointer(x1, y1, true);
    frame(NULL);
    ui_pointer(x1, y1, false);
    frame(NULL);
}

int main(void) {
    printf("=== em-test: the multi-line editor ===\n");
    scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA);
    ui_theme_use_dark(true);

    /* Without a font every text width is zero, and the editor's column
     * hit-testing would agree with any expectation at all. See ui/testfont.h. */
    static struct testfont TF;
    uint32_t fh = font_load(TF.data, testfont_build(&TF));
    if (!fh) { printf("  FAIL: could not build the test font\n"); return 1; }
    ui_theme_set_fonts(fh, fh);
    ui_clock_provider(fake_clock);
    ui_clipboard_provider(clip_set, clip_get);

    #define LEFT   "\x11"
    #define RIGHT  "\x12"
    #define HOME   "\x02"
    #define END    "\x05"
    #define DEL    "\x7F"
    #define SLEFT  "\xF8"
    #define SRIGHT "\xF9"
    #define SHOME  "\xFA"
    #define SEND   "\xFB"
    #define SALL   "\xFC"
    #define COPY   "\xFD"
    #define CUT    "\xFE"
    #define PASTE  "\xFF"

    DOC[0] = 0; CUR = 0;
    frame(NULL);
    click_in();
    frame("hello");
    CHECK(!strcmp(DOC, "hello"), "the editor takes a click and then takes typing");

    /* THE REGRESSION THIS FILE WAS WRITTEN FOR. Every command byte must be
     * CONSUMED, never inserted: the default branch takes any byte >= 0x80
     * because that is how é arrives, and these are above it. */
    frame(COPY CUT SALL SLEFT SRIGHT SHOME SEND);
    frame(HOME);
    CHECK(!strcmp(DOC, "hello"), "no command byte is ever inserted as text");

    frame(END);
    frame(SHOME COPY);
    CHECK(!strcmp(clip_text(), "hello"), "Shift+Home selects the line and copy takes it");

    frame(END);
    frame("\n");
    frame(PASTE);
    CHECK(!strcmp(DOC, "hello\nhello"), "paste puts it back, on a new line");

    /* A PASTE CARRIES ITS NEWLINES. The runtime's app-wide replay flattens
     * them to spaces, which is right for a terminal and wrong here -- an
     * editor that silently collapses a pasted block onto one line is an
     * editor that eats your text. This one pastes itself. */
    frame(SALL COPY);
    frame(END);
    frame(PASTE);
    CHECK(!strcmp(DOC, "hello\nhellohello\nhello"),
          "a pasted block keeps its newlines instead of being flattened");

    DOC[0] = 0; CUR = 0; g_clip_n = 0;
    frame(NULL);
    frame("abc");
    frame(SLEFT SLEFT);
    frame("Z");
    CHECK(!strcmp(DOC, "aZ"), "typing over a selection replaces it");

    frame(SHOME);
    frame("\b");
    CHECK(DOC[0] == 0, "backspace over a selection removes the whole selection");

    /* A plain arrow collapses to the edge it moves toward. */
    DOC[0] = 0; CUR = 0; frame(NULL);
    frame("abcdef");
    frame(SHOME);                       /* all selected, caret at 0 */
    frame(RIGHT);                       /* collapse to the right edge */
    frame("!");
    CHECK(!strcmp(DOC, "abcdef!"), "a plain arrow collapses the selection to the edge it moves toward");

    frame(SALL);
    frame(LEFT);
    frame("^");
    CHECK(!strcmp(DOC, "^abcdef!"), "and Left collapses it to the other one");

    /* Cut, and a selection spanning a newline. */
    DOC[0] = 0; CUR = 0; frame(NULL);
    frame("one\ntwo");
    frame(SHOME SLEFT SHOME);           /* back across the newline to the start */
    frame(CUT);
    CHECK(!strcmp(clip_text(), "one\ntwo") && DOC[0] == 0,
          "a selection spanning a newline cuts both lines");

    /* Multi-byte characters travel whole: one Shift+Left selects all of an é. */
    DOC[0] = 0; CUR = 0; g_clip_n = 0; frame(NULL);
    frame("caf\xC3\xA9");
    frame(SLEFT COPY);
    CHECK(!strcmp(clip_text(), "\xC3\xA9"), "a selection over é carries BOTH its bytes");

    /* ---- THE MOUSE: placing the caret, and dragging a selection ---------
     *
     * Before this the editor answered a click by taking focus and nothing
     * else, so the caret stayed wherever it was and the only way to reach a
     * line was to arrow to it -- in a CODE EDITOR.
     *
     * The y of a line is the surface's top padding plus the line pitch, which
     * is the drawn text size plus the 5px the lines are spaced by. Computed
     * from the theme rather than hardcoded, so this test does not quietly
     * become a test of one font size. */
    {
        const struct ui_theme *th = ui_theme();
        float lh = th->text_body + 5.0f;
        #define LINE_Y(n) (th->sp2 + lh * (n) + lh * 0.5f)

        DOC[0] = 0; CUR = 0; g_clip_n = 0;
        frame(NULL);
        strcpy(DOC, "alpha\nbravo\ncharlie");
        frame(NULL);

        /* Now that there IS a font, a column can be aimed at exactly: every
         * character is half the text size wide, and the text starts one left
         * padding in. */
        float adv = testfont_advance(th->text_body);
        #define COL_X(n) (th->sp3 + adv * (float)(n) + adv * 0.5f)

        click_at(0, LINE_Y(1));
        CHECK(CUR == 6, "a click lands on the line it was aimed at");
        frame("X");
        CHECK(!strcmp(DOC, "alpha\nXbravo\ncharlie"), "and typing goes there");

        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; frame(NULL);
        click_at(COL_X(2), LINE_Y(1));
        CHECK(CUR == 6 + 3, "and on the COLUMN it was aimed at");
        click_at(COL_X(2), LINE_Y(2));
        CHECK(CUR == 12 + 3, "column and line together");

        /* Far right of a line is its END, not the start of the next one. */
        click_at(390, LINE_Y(0));
        CHECK(CUR == 5, "clicking past the end of a line stops at the end of it");

        /* Below the last line is the last line, not nothing. */
        click_at(390, LINE_Y(9));
        CHECK(CUR == (int)strlen(DOC), "clicking below the text lands at the very end");

        /* A DRAG SELECTS. Pointer capture means it keeps extending even once
         * the pointer has left the editor, which is why the release here is
         * far outside it. */
        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; frame(NULL);
        drag(0, LINE_Y(0), 390, LINE_Y(1));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha\nbravo"), "dragging across two lines selects both");

        strcpy(DOC, "alpha\nbravo\ncharlie"); CUR = 0; g_clip_n = 0; frame(NULL);
        drag(0, LINE_Y(0), 900, LINE_Y(40));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha\nbravo\ncharlie"),
              "a drag that leaves the editor keeps going to the end");

        /* And the selection a drag made is a real one: typing replaces it. */
        frame("!");
        CHECK(!strcmp(DOC, "!"), "typing replaces what the drag selected");

        /* Double-click takes a word; a third takes the LINE, not the whole
         * document -- a triple-click in a file that selected everything would
         * be a nasty surprise. */
        strcpy(DOC, "alpha bravo\ncharlie"); CUR = 0; g_clip_n = 0; frame(NULL);
        g_ms += 900;  click_fast(COL_X(8), LINE_Y(0));
        g_ms += 50;   click_fast(COL_X(8), LINE_Y(0));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "bravo"), "a double-click in the editor takes the word");

        g_ms += 50;   click_fast(COL_X(8), LINE_Y(0));
        frame(COPY);
        CHECK(!strcmp(clip_text(), "alpha bravo"),
              "a third click takes the LINE, not the whole document");
        #undef COL_X
        #undef LINE_Y
    }

    printf("=== em-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
