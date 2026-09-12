/* ui/kit/kit_test.c -- the text field's keyboard behaviour, on the host.
 * make kit-test  -> exits 0 iff every claim holds.
 *
 * What a login form needs from a field and did not have: to open ready to
 * type (autofocus), Tab to the next field with the keys typed after the Tab
 * following it there, wrap from the last field to the first, Return reported
 * by the field that saw it -- and focus let go when the focused field is no
 * longer on screen, so the next page can take it. */
#include "ui.h"
#include "scene.h"
#include "layout.h"
#include "kit.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
                              else { printf("  ok:   %s\n", (msg)); } } while (0)

static struct scene_arena  SA;
static struct layout_arena LA;

/* A CLIPBOARD OF OUR OWN. The kit takes one as two function pointers precisely
 * so it never has to know about the kernel -- which is what lets copy and cut
 * be tested here, on the host, in a millisecond, instead of only in QEMU. */
static char g_clip[256];
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
    static char out[257];
    memcpy(out, g_clip, g_clip_n); out[g_clip_n] = 0; return out;
}
static char A[32], B[32], C[32];
static bool subA, subB, subC;
static bool show_b = true, autofocus = true;
static uint64_t page = 0x1111;            /* a keyed container: a new key is a new page */

static void form(void) {
    ui_begin_vstack(page);
      if (autofocus) ui_text_field_autofocus();
      ui_text_field(A, sizeof A, "a");  subA = ui_text_field_submitted();
      if (show_b) { ui_text_field(B, sizeof B, "b");  subB = ui_text_field_submitted(); }
      ui_password_field(C, sizeof C, "c"); subC = ui_text_field_submitted();
    ui_end_stack();
}
static void frame(const char *keys) {
    for (const char *k = keys; k && *k; k++) ui_input_char(*k);
    ui_frame_begin(); form(); ui_frame_end();
    ui_run_layout(400, 300);
}

int main(void) {
    printf("=== kit-test: the text field and the keyboard ===\n");
    scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA);
    ui_clipboard_provider(clip_set, clip_get);

    frame(NULL);
    CHECK(ui_any_focus(), "a form with autofocus opens with a field focused");
    frame("tester");
    CHECK(!strcmp(A, "tester"), "the keys go to the FIRST field");

    frame("\t");
    frame("pass");
    CHECK(!strcmp(B, "pass") && !strcmp(A, "tester"), "Tab moves to the next field; typing lands there");

    frame("\tword\n");
    CHECK(!strcmp(C, "word"), "Tab then keys in ONE frame: the keys after the Tab follow it");
    CHECK(subC && !subA && !subB, "Return is reported by the field that saw it, and only by it");
    frame(NULL);
    CHECK(!subA && !subB && !subC, "and only once: a submit is an edge");

    frame("\t");
    frame("x");
    CHECK(!strcmp(A, "testerx"), "Tab in the LAST field wraps to the first");

    /* Hiding one field is NOT a new page: identity is positional, so the
     * field after it inherits its instance -- and its focus. */
    frame("\t");                          /* focus to B */
    show_b = false;
    frame(NULL);
    CHECK(ui_any_focus(), "a field that disappears mid-form hands its place (and focus) to the next");
    show_b = true;

    /* A NEW PAGE -- a differently keyed container, every widget new: the
     * focused one is gone for good. */
    autofocus = false;
    page = 0x2222;
    frame(NULL);
    frame(NULL);
    CHECK(!ui_any_focus(), "focus is let go when the focused field is no longer on screen");
    autofocus = true;
    A[0] = 0;
    frame(NULL);
    CHECK(ui_any_focus(), "so the next page's autofocus takes it");
    frame("z");
    CHECK(!strcmp(A, "z"), "and typing goes to that page's first field");

    /* ---- UTF-8: the field has to hold a language, not just ASCII --------
     *
     * The key stream carries UTF-8 now (the keyboard driver encodes codepoints
     * rather than truncating them to 7 bits), so é arrives as two bytes and €
     * as three. This field used to reject every byte outside 32..126 -- which
     * silently dropped every accented character between the keyboard and the
     * screen -- and its backspace deleted ONE BYTE, which cuts a two-byte
     * character in half and leaves something the renderer draws as a
     * replacement box.
     *
     * Typed here as the exact bytes the driver produces, because that is what
     * the field actually receives. */
    page = 0x3333; autofocus = true; A[0] = 0;
    frame(NULL); frame(NULL);
    frame("caf\xC3\xA9");                 /* c a f + U+00E9 */
    CHECK(!strcmp(A, "caf\xC3\xA9"), "an accented character survives into the field (café)");

    frame("\b");
    CHECK(!strcmp(A, "caf"), "one backspace removes the WHOLE é, not half of it");

    A[0] = 0; frame(NULL);
    frame("\xE2\x82\xAC");               /* U+20AC EURO SIGN, three bytes */
    CHECK(!strcmp(A, "\xE2\x82\xAC"), "a three-byte character (€) survives too");
    frame("\b");
    CHECK(A[0] == 0, "and one backspace removes all three of its bytes");

    /* ---- THE CARET: editing somewhere other than the end ----------------
     *
     * Until it existed, this field could only be appended to and backspaced
     * from the end -- so fixing a typo three characters back meant deleting
     * everything after it, in every filename box and search box in the system.
     * The navigation keys arrive as bytes in this same stream (UI_KEY_*), which
     * is why they can be typed here exactly as the driver sends them. */
    #define LEFT  "\x11"
    #define RIGHT "\x12"
    #define HOME  "\x02"
    #define END   "\x05"
    #define DEL   "\x7F"

    page = 0x4444; autofocus = true; A[0] = 0;
    frame(NULL); frame(NULL);
    frame("world");
    frame(HOME "hello ");
    CHECK(!strcmp(A, "hello world"), "Home puts the caret at the front and typing INSERTS there");

    frame(END "!");
    CHECK(!strcmp(A, "hello world!"), "End goes back to the end");

    /* "hello world!" -- three lefts put the caret before the l of "world",
     * so backspace takes the r. */
    frame(LEFT LEFT LEFT "\b");
    CHECK(!strcmp(A, "hello wold!"), "backspace takes the character BEFORE the caret, mid-string");

    frame(DEL);
    CHECK(!strcmp(A, "hello wod!"), "Delete takes the one after it -- a key that used to do nothing");

    frame(HOME DEL DEL DEL DEL DEL DEL);
    CHECK(!strcmp(A, "wod!"), "Delete at the front eats forwards");

    /* A caret is a CHARACTER position, not a byte position: the stream is
     * UTF-8, and an arrow key that moved one byte would step into the middle
     * of an é and let a backspace cut it in half. */
    A[0] = 0; frame(NULL);
    frame("caf\xC3\xA9s");                /* c a f é s */
    frame(LEFT LEFT);                      /* over the s, then over the whole é */
    frame("X");
    CHECK(!strcmp(A, "cafX\xC3\xA9s"), "Left steps over a whole é, not one of its bytes");

    frame(RIGHT);                          /* past the é, in one press */
    frame("Y");
    CHECK(!strcmp(A, "cafX\xC3\xA9Ys"), "and Right steps over all of it too");

    /* One left puts the caret between the é and the Y, so backspace takes the
     * whole é -- both its bytes, in one press. */
    frame(LEFT "\b");
    CHECK(!strcmp(A, "cafXYs"), "backspace over a multi-byte character removes all of it");

    /* FOCUSING A FIELD PUTS THE CARET AT THE END OF ITS TEXT, and that is the
     * documented behaviour rather than a limitation worked around: there is one
     * caret because exactly one field is focused, and arriving at the end is
     * what someone Tabbing into a field to add to it expects. Pointing at a
     * spot is how you get anywhere else, and a click does exactly that. */
    A[0] = 0; B[0] = 0; frame(NULL);
    frame("abc");
    frame(HOME);                           /* caret to the front of A */
    frame("\t");                           /* to B */
    frame("zz");
    frame("\t\t");                         /* wrap back round to A */
    frame("Q");
    CHECK(!strcmp(A, "abcQ"), "coming back to a field puts the caret at the end of its text");
    CHECK(!strcmp(B, "zz"), "and the other field kept its own text");

    /* ---- SELECTION, AND THE CLIPBOARD ----------------------------------
     *
     * These commands ride the SAME byte stream as the text, at 0xF8..0xFF --
     * byte values that can never occur in valid UTF-8, which is what makes it
     * safe to put them there and what keeps them in ORDER with typed
     * characters. They come from the GUI key rather than Ctrl because Ctrl+C is
     * this OS's console interrupt and is consumed before any app sees it. */
    #define SLEFT  "\xF8"
    #define SRIGHT "\xF9"
    #define SHOME  "\xFA"
    #define SEND   "\xFB"
    #define SALL   "\xFC"
    #define COPY   "\xFD"
    #define CUT    "\xFE"

    page = 0x5555; autofocus = true; A[0] = 0;
    frame(NULL); frame(NULL);
    frame("hello world");
    frame(SLEFT SLEFT SLEFT SLEFT SLEFT);   /* select "world" backwards */
    frame(COPY);
    CHECK(!strcmp(clip_text(), "world"), "Shift+Left selects, and copy puts it on the clipboard");
    CHECK(!strcmp(A, "hello world"), "copying does not change the text");

    frame("W");
    CHECK(!strcmp(A, "hello W"), "typing REPLACES the selection");

    /* Step off the end first, so this tests that Shift+Home extends from where
     * the CARET is rather than simply selecting everything. */
    frame(LEFT);                            /* caret between "hello " and "W" */
    frame(SHOME);                           /* select back to the start */
    frame(CUT);
    CHECK(!strcmp(clip_text(), "hello ") && A[0] == 'W' && A[1] == 0,
          "Shift+Home selects to the front from the caret, and cut removes exactly that");

    /* Select-all, then one keystroke: the "clear this field" everyone does. */
    A[0] = 0; frame(NULL);
    frame("something long");
    frame(SALL "x");
    CHECK(!strcmp(A, "x"), "select-all then a key replaces the whole field");

    /* An arrow with a selection COLLAPSES it to the edge it moves toward,
     * rather than moving the caret from wherever it happened to be. */
    A[0] = 0; frame(NULL);
    frame("abcdef");
    frame(SHOME);                           /* all of it selected, caret at 0 */
    frame(RIGHT);                           /* collapse to the RIGHT edge */
    frame("Z");
    CHECK(!strcmp(A, "abcdefZ"), "a plain arrow collapses the selection to the edge it moves toward");

    frame(SALL);
    frame(LEFT);                            /* collapse to the LEFT edge */
    frame("Q");
    CHECK(!strcmp(A, "QabcdefZ"), "and Left collapses it to the other one");

    /* Backspace with a selection deletes the selection, not one character. */
    A[0] = 0; frame(NULL);
    frame("keep this");
    frame(SLEFT SLEFT SLEFT SLEFT);
    frame("\b");
    CHECK(!strcmp(A, "keep "), "backspace over a selection removes the whole selection");

    /* A selection spanning a multi-byte character travels in whole characters,
     * so what reaches the clipboard is text and not a broken fragment. */
    A[0] = 0; frame(NULL);
    frame("caf\xC3\xA9");
    frame(SLEFT SLEFT);                     /* the é and the f */
    frame(COPY);
    CHECK(!strcmp(clip_text(), "f\xC3\xA9"), "a selection over é copies BOTH its bytes");

    /* A password field keeps its secret: the caret and the selection work, but
     * the text never leaves it. */
    g_clip_n = 0;
    C[0] = 0; page = 0x6666; autofocus = false; frame(NULL); frame(NULL);
    frame("\t\t");                          /* into the password field */
    frame("secret");
    frame(SALL COPY);
    CHECK(g_clip_n == 0, "a masked field refuses to copy its text");

    printf("=== kit-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
