/* screentext_test.c -- selection, find and links over the screen model
 * (user/terminal/screentext.c), host-run. */
#include "screentext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)
static struct vt v;
static void start(int cols, int rows) { vt_free(&v); vt_init(&v, cols, rows, NULL, NULL); }
static void feed(const char *s) { vt_feed(&v, s, strlen(s)); }
static struct term_point P(int row, int col) { struct term_point p = { vt_row_id(&v, row, 0), col }; return p; }
static char buf[4096];
static const char *selected(const struct term_selection *s) { term_select_text(s, &v, buf, sizeof buf); return buf; }
static bool same(const char *a, const char *b) {
    if (strcmp(a, b)) printf("    got \"%s\", want \"%s\"\n", a, b);
    return !strcmp(a, b);
}

int main(void) {
    printf("=== screen text: selection, find, links ===\n");
    struct term_selection s = {0};

    printf("logical lines:\n");
    start(10, 6);
    feed("0123456789abcdefghij-end\r\nnext");
    struct term_text t = {0};
    CHECK(term_text_line(&v, P(1, 0).row, &t) && t.first == P(0, 0).row && t.last == P(2, 0).row && t.n == 24,
          "a wrapped line is read whole from any of its rows");
    CHECK(term_text_line(&v, P(3, 0).row, &t) && t.n == 4 && t.cp[0] == 'n', "the next line starts where it starts");
    term_text_free(&t);

    printf("selection:\n");
    start(30, 6);
    feed("hello world   \r\nsecond line\r\n/usr/local/bin and (x)\r\n");
    term_select_start(&s, &v, P(0, 6), TERM_SELECT_CHARS);
    term_select_extend(&s, &v, P(1, 6));
    CHECK(same(selected(&s), "world\nsecond"), "a drag: across a line end, trailing blanks dropped");
    term_select_start(&s, &v, P(1, 6), TERM_SELECT_CHARS);
    term_select_extend(&s, &v, P(0, 6));
    CHECK(same(selected(&s), "world\nsecond"), "dragged backward, the same text");
    term_select_start(&s, &v, P(2, 7), TERM_SELECT_WORDS);
    CHECK(same(selected(&s), "/usr/local/bin"), "a double click takes a whole path");
    term_select_start(&s, &v, P(2, 20), TERM_SELECT_WORDS);
    CHECK(same(selected(&s), "x"), "but not the parenthesis around a word");
    term_select_start(&s, &v, P(2, 21), TERM_SELECT_WORDS);
    CHECK(same(selected(&s), ")"), "a punctuation mark alone is itself");
    term_select_start(&s, &v, P(0, 1), TERM_SELECT_WORDS);
    term_select_extend(&s, &v, P(1, 2));
    CHECK(same(selected(&s), "hello world\nsecond"), "dragging after a double click goes a word at a time");
    term_select_start(&s, &v, P(1, 3), TERM_SELECT_LINES);
    CHECK(same(selected(&s), "second line"), "a triple click takes the line");
    start(10, 6);
    feed("0123456789abcdefghij-end\r\nnext");
    term_select_start(&s, &v, P(1, 4), TERM_SELECT_LINES);
    CHECK(same(selected(&s), "0123456789abcdefghij-end"), "a wrapped line, whole and joined, from its middle row");
    term_select_start(&s, &v, P(0, 2), TERM_SELECT_WORDS);
    CHECK(same(selected(&s), "0123456789abcdefghij-end"), "a word runs across the wrap");
    term_select_start(&s, &v, P(0, 2), TERM_SELECT_BLOCK);
    term_select_extend(&s, &v, P(2, 5));
    CHECK(same(selected(&s), "234\ncde\nnd"), "Alt-drag: a rectangle, one row a line");
    int x0, x1;
    CHECK(term_select_span(&s, &v, P(1, 0).row, v.cols, &x0, &x1) && x0 == 2 && x1 == 5, "and its columns on each row");
    term_select_all(&s, &v);
    CHECK(same(selected(&s), "0123456789abcdefghij-end\nnext"), "select all: everything, no trailing blank lines");
    /* a selection stays on its text while output scrolls it into the scrollback */
    start(20, 3);
    feed("keep this word\r\n");
    term_select_start(&s, &v, P(0, 5), TERM_SELECT_WORDS);
    int64_t was = s.anchor.row;
    for (int i = 0; i < 5; i++) feed("more output\r\n");
    CHECK(s.anchor.row == was && vt_row_in_view(&v, was, 0) == -1, "the selected row scrolled out of view");
    CHECK(same(selected(&s), "this"), "and the selection still reads its word");
    CHECK(vt_row_in_view(&v, was, 4) == 0 && vt_row_in_view(&v, was, 3) == -1, "where the scrollback view shows it");
    term_select_clear(&s);
    CHECK(!term_select_span(&s, &v, was, 20, &x0, &x1) && !term_select_text(&s, &v, buf, sizeof buf), "cleared: nothing");

    printf("find:\n");
    start(16, 4);
    feed("alpha Beta\r\ngamma beta\r\ndelta\r\nbeta two\r\nlast");
    struct term_match m;
    struct term_point bottom = { vt_row_id(&v, v.rows - 1, 0), v.cols };
    /* "alpha Beta" went to the scrollback; the screen shows the other four */
    int64_t history = vt_row_id(&v, 0, 1);
    CHECK(term_find(&v, "BETA", bottom, true, &m) && m.start.row == P(2, 0).row && m.start.col == 0 && m.end.col == 4,
          "backward from the bottom: the last one, ignoring case");
    CHECK(term_find(&v, "beta", m.start, true, &m) && m.start.row == P(0, 0).row && m.start.col == 6,
          "again: the one before it");
    CHECK(term_find(&v, "beta", m.start, true, &m) && m.start.col == 6 && m.start.row == history,
          "and the one before that, in the scrollback");
    CHECK(!term_find(&v, "beta", m.start, true, &m), "then none");
    struct term_point top = { history, -1 };
    CHECK(term_find(&v, "beta", top, false, &m) && m.start.row == history && m.start.col == 6, "forward from the top: the first");
    CHECK(!term_find(&v, "nowhere", bottom, true, &m) && !term_find(&v, "", bottom, true, &m), "no match, no needle: nothing");
    start(8, 4);
    feed("xxxxxxneedle zz");
    CHECK(term_find(&v, "needle", P(3, 8), true, &m) && m.start.row == P(0, 0).row && m.start.col == 6 && m.end.row == P(1, 0).row && m.end.col == 4,
          "a match across a wrap");
    int sx0[4], sx1[4];
    CHECK(term_find_spans(&v, "needle", P(0, 0).row, sx0, sx1, 4) == 1 && sx0[0] == 6 && sx1[0] == 8 &&
          term_find_spans(&v, "needle", P(1, 0).row, sx0, sx1, 4) == 1 && sx0[0] == 0 && sx1[0] == 4,
          "its spans, row by row");
    start(20, 2);
    feed("caf\xc3\xa9 CAF\xc3\xa9");
    CHECK(term_find_spans(&v, "caf\xc3\xa9", P(0, 0).row, sx0, sx1, 4) == 2, "UTF-8 needles; ASCII case folds");

    printf("links:\n");
    char url[256];
    start(60, 6);
    feed("see https://emblink.invalid/docs/a_(b). ok\r\nmail mailto:me@example.invalid, or www.example.invalid/x\r\n");
    CHECK(term_link_at(&v, P(0, 10), url, sizeof url, &m) && same(url, "https://emblink.invalid/docs/a_(b)") &&
          m.start.col == 4 && m.end.col == 38, "a URL in text: balanced parentheses kept, the full stop left");
    CHECK(!term_link_at(&v, P(0, 1), url, sizeof url, &m), "beside it, no link");
    CHECK(term_link_at(&v, P(1, 8), url, sizeof url, &m) && same(url, "mailto:me@example.invalid"), "mailto:, without the comma");
    CHECK(term_link_at(&v, P(1, 40), url, sizeof url, &m) && same(url, "http://www.example.invalid/x"), "www. becomes http://");
    feed("\033]8;id=1;https://target.invalid/\033\\click here\033]8;;\033\\ plain");
    CHECK(term_link_at(&v, P(2, 3), url, sizeof url, &m) && same(url, "https://target.invalid/") && m.start.col == 0 && m.end.col == 10,
          "an OSC 8 link: its target, over the text it covers");
    CHECK(!term_link_at(&v, P(2, 13), url, sizeof url, &m), "after it, plain text");
    start(12, 4);
    feed("go https://long.invalid/path now");
    CHECK(term_link_at(&v, P(1, 2), url, sizeof url, &m) && same(url, "https://long.invalid/path") && m.start.row == P(0, 0).row &&
          m.end.row == P(2, 0).row, "a URL wrapped over three rows is one link");

    vt_free(&v);
    printf("=== screentext-test: %s (%u checks, %u failures) ===\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
