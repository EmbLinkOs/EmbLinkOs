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

    printf("=== kit-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
