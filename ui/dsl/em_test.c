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

int main(void) {
    printf("=== em-test: the multi-line editor ===\n");
    scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA);
    ui_theme_use_dark(true);
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

    printf("=== em-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
