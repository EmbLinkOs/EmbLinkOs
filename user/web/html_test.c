/* user/web/html_test.c -- the parser's host test.
 *
 * Runs on the DEVELOPMENT machine (`make html-test`), not the OS. A parser is
 * the one part of a browser that can be fully exercised without a network, a
 * window or a font -- so it should be, and in seconds, rather than through a
 * two-minute image build and a boot.
 */
#include <stdio.h>
#include <string.h>
#include "html.h"

static int failures;
#define CHECK(c, what) do {                                            \
        if (c) printf("  ok:   %s\n", what);                           \
        else { printf("  FAIL: %s\n", what); failures++; }             \
    } while (0)

static struct html_node NODES[4096];
static char             STRS[65536];
static struct html_doc  D;

static int parse(const char *src) {
    return html_parse(&D, src, strlen(src), NODES, 4096, STRS, sizeof STRS);
}

/* first element with this tag, depth-first */
static int find(int at, const char *tag) {
    if (at < 0) return -1;
    if (D.nodes[at].kind == HTML_ELEM && !strcmp(D.nodes[at].tag, tag)) return at;
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling) {
        int r = find(c, tag);
        if (r >= 0) return r;
    }
    return -1;
}
static int count(int at, const char *tag) {
    if (at < 0) return 0;
    int n = (D.nodes[at].kind == HTML_ELEM && !strcmp(D.nodes[at].tag, tag)) ? 1 : 0;
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling)
        n += count(c, tag);
    return n;
}
/* concatenated text under a node */
static void gather(int at, char *out, size_t cap) {
    if (at < 0) return;
    if (D.nodes[at].kind == HTML_TEXT && D.nodes[at].text) {
        size_t l = strlen(out), a = strlen(D.nodes[at].text);
        if (l + a + 1 < cap) memcpy(out + l, D.nodes[at].text, a + 1);
        return;
    }
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling)
        gather(c, out, cap);
}

static void t1_structure(void) {
    printf("T1 elements, nesting, text:\n");
    int r = parse("<html><body><h1>Title</h1><p>Hello <b>world</b>.</p></body></html>");
    CHECK(r >= 0, "parsed");
    CHECK(find(r, "h1") >= 0, "h1 found");
    CHECK(find(r, "b")  >= 0, "nested b found");
    char buf[256] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "Hello world."), "paragraph text reassembles");
}

static void t2_implicit_close(void) {
    printf("T2 implicit end tags (the one that ruins real pages):\n");
    int r = parse("<body><p>one<p>two<p>three</body>");
    CHECK(count(r, "p") == 3, "three sibling paragraphs, not nested");
    int p1 = find(r, "p");
    CHECK(D.nodes[p1].first_child >= 0 &&
          D.nodes[D.nodes[p1].first_child].kind == HTML_TEXT, "first p holds text");
    CHECK(find(D.nodes[p1].first_child, "p") < 0, "first p does NOT contain a p");

    r = parse("<ul><li>a<li>b<li>c</ul>");
    CHECK(count(r, "li") == 3, "three sibling list items");
}

static void t3_void_and_attrs(void) {
    printf("T3 void elements and attributes:\n");
    int r = parse("<p>a<br>b</p><img src=\"/pic.png\"><a href='/x'>link</a>");
    CHECK(count(r, "br") == 1, "br parsed");
    int img = find(r, "img");
    CHECK(img >= 0 && D.nodes[img].href && !strcmp(D.nodes[img].href, "/pic.png"),
          "img src captured (double-quoted)");
    int a = find(r, "a");
    CHECK(a >= 0 && D.nodes[a].href && !strcmp(D.nodes[a].href, "/x"),
          "a href captured (single-quoted)");
    char buf[64] = {0};
    gather(a, buf, sizeof buf);
    CHECK(!strcmp(buf, "link"), "anchor text");
    /* a void element must not swallow what follows it */
    CHECK(find(D.nodes[find(r,"br")].first_child, "a") < 0, "br has no children");
}

static void t4_entities(void) {
    printf("T4 character references:\n");
    int r = parse("<p>a &amp; b &lt;tag&gt; &#65; &#x42; &quot;q&quot;</p>");
    char buf[256] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "a & b <tag> A B \"q\""), "named, decimal and hex decode");
    r = parse("<p>bare &notanentity; stays</p>");
    buf[0] = 0; gather(find(r, "p"), buf, sizeof buf);
    CHECK(strstr(buf, "&notanentity;") != 0, "an unknown entity is left literal");
}

static void t5_script_style(void) {
    printf("T5 script/style contents are not markup:\n");
    int r = parse("<body><script>var x = '<p>fake</p>';</script><p>real</p></body>");
    CHECK(count(r, "p") == 1, "the <p> inside the script did not become an element");
    char buf[256] = {0};
    gather(r, buf, sizeof buf);
    CHECK(!strstr(buf, "var x"), "script source is not shown as text");
    CHECK(strstr(buf, "real") != 0, "content after the script still parses");

    r = parse("<style>p{color:red}</style><p>hi</p>");
    buf[0] = 0; gather(r, buf, sizeof buf);
    CHECK(!strstr(buf, "color:red"), "stylesheet is not shown as text");
}

static void t6_whitespace(void) {
    printf("T6 whitespace collapsing:\n");
    int r = parse("<p>one\n   two\t\tthree</p>");
    char buf[128] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "one two three"), "runs collapse to one space");
    r = parse("<p>   leading and trailing   </p>");
    buf[0] = 0; gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "leading and trailing"), "edges trimmed");
}

static void t7_malformed(void) {
    printf("T7 malformed input does not derail the parse:\n");
    int r = parse("<p>text</div></span><p>after");
    CHECK(count(r, "p") == 2, "stray close tags ignored, both paragraphs kept");
    r = parse("<p>unclosed <b>bold");
    CHECK(r >= 0 && find(r, "b") >= 0, "unclosed tags at EOF still produce a tree");
    r = parse("<!-- <p>commented</p> --><p>live</p>");
    CHECK(count(r, "p") == 1, "commented markup is not parsed");
    r = parse("<!doctype html><p>x</p>");
    CHECK(count(r, "p") == 1, "doctype skipped");
}

static void t8_urls(void) {
    printf("T8 URL resolution (link following lives or dies here):\n");
    char out[512];
    html_resolve_url("http://a.example/dir/page.html", "http://b.example/x", out, sizeof out);
    CHECK(!strcmp(out, "http://b.example/x"), "absolute passes through");
    html_resolve_url("http://a.example/dir/page.html", "/root.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/root.html"), "root-relative");
    html_resolve_url("http://a.example/dir/page.html", "next.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/dir/next.html"), "relative to the DIRECTORY");
    html_resolve_url("http://a.example", "x.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/x.html"), "bare host gets a slash");
    html_resolve_url("http://a.example/dir/", "sub/y.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/dir/sub/y.html"), "relative with a subpath");
}

static void t9_bounded(void) {
    printf("T9 bounded arenas (this parses input from the network):\n");
    static struct html_node few[8];
    static char small[64];
    struct html_doc d;
    const char *src = "<body><p>a</p><p>b</p><p>c</p><p>d</p><p>e</p><p>f</p></body>";
    int r = html_parse(&d, src, strlen(src), few, 8, small, sizeof small);
    CHECK(r >= 0, "returns a usable root even when the arena is too small");
    CHECK(d.truncated == 1, "truncation is REPORTED, not silent");
    CHECK(d.n <= 8, "never exceeds the node arena");
}

int main(void) {
    printf("=== html-test ===\n");
    t1_structure(); t2_implicit_close(); t3_void_and_attrs(); t4_entities();
    t5_script_style(); t6_whitespace(); t7_malformed(); t8_urls(); t9_bounded();
    printf("=== html-test: %s (%d failures) ===\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
