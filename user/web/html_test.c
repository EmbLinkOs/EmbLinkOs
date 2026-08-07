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
#include "url.h"
#include "style.h"
#include "css.h"

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

/* T10-T11 cover url.c: what a location IS, before anything fetches it. Pure
 * string work, so it belongs in the fast host loop rather than in a boot. */
static void t10_url_parse(void) {
    printf("T10 url_parse (a path and a URL both go in the address bar):\n");
    struct url u;

    CHECK(url_parse("http://example.com/a/b?q=1", &u) == 0, "http parses");
    CHECK(u.kind == URL_HTTP && u.port == 80, "default port 80");
    CHECK(!strcmp(u.host, "example.com"), "host split off");
    CHECK(!strcmp(u.path, "/a/b?q=1"), "the query stays part of the request target");

    CHECK(url_parse("https://a.test", &u) == 0, "https with no path parses");
    CHECK(u.kind == URL_HTTPS && u.port == 443, "default port 443");
    CHECK(!strcmp(u.path, "/"), "an empty path becomes /");

    CHECK(url_parse("http://10.0.2.2:8000/hello.html", &u) == 0, "explicit port parses");
    CHECK(u.port == 8000 && !strcmp(u.host, "10.0.2.2"), "port split from a literal host");

    CHECK(url_parse("/system/web/index.html", &u) == 0, "a bare path is a LOCAL location");
    CHECK(u.kind == URL_LOCAL && !strcmp(u.path, "/system/web/index.html"), "kept verbatim");

    CHECK(url_parse("file:///data/x.html", &u) == 0, "file:// parses");
    CHECK(u.kind == URL_LOCAL && !strcmp(u.path, "/data/x.html"), "file:// yields the path");

    CHECK(url_parse("ftp://x/y", &u) != 0, "an unsupported scheme is refused");
    CHECK(url_parse("nonsense", &u) != 0, "a bare word is refused, not guessed at");
    CHECK(url_parse("http://", &u) != 0, "a URL with no host is refused");
}

static void t11_url_resolve(void) {
    printf("T11 url_resolve (one rule, two worlds):\n");
    char out[512];

    CHECK(url_resolve("http://h/a/b.html", "c.html", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/a/c.html"), "network base + relative -> sibling");
    CHECK(url_resolve("http://h/a/b.html", "/z.html", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/z.html"), "network base + root-relative -> host root");
    CHECK(url_resolve("http://h/a/b.html", "https://o/x", out, sizeof out) == 0 &&
          !strcmp(out, "https://o/x"), "an absolute href ignores the base");

    /* the local half, which the network resolver cannot do: it has no "://" to
     * anchor on, and a leading '/' IS the root rather than being relative to one */
    CHECK(url_resolve("/system/web/index.html", "about.html", out, sizeof out) == 0 &&
          !strcmp(out, "/system/web/about.html"), "local base + relative -> sibling file");
    CHECK(url_resolve("/system/web/index.html", "/data/x.html", out, sizeof out) == 0 &&
          !strcmp(out, "/data/x.html"), "local base + absolute path -> that path");
    CHECK(url_resolve("/system/web/index.html", "http://h/x", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/x"), "a local page can link OUT to the network");

    CHECK(url_resolve("http://h/a", "", out, sizeof out) != 0, "an empty href resolves to nothing");
}

/* ======================= CSS (B5) ======================================= */

static struct html_node CN[512];
static char CS[16384];
static struct html_doc CD;

static int cparse(const char *src) {
    return html_parse(&CD, src, strlen(src), CN, 512, CS, sizeof CS);
}
/* find the first element with this tag */
static int find_tag(const char *tag) {
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, tag)) return i;
    return -1;
}

static void t12_attrs_and_style_block(void) {
    printf("T12 the parser keeps what CSS needs:\n");
    cparse("<style>p{color:red}</style>"
           "<p class='lead big' id='intro' style='font-weight:bold'>hi</p>");
    int p = find_tag("p");
    CHECK(p >= 0, "the element parsed");
    CHECK(CD.nodes[p].klass && !strcmp(CD.nodes[p].klass, "lead big"), "class is kept verbatim");
    CHECK(CD.nodes[p].id && !strcmp(CD.nodes[p].id, "intro"), "id is kept");
    CHECK(CD.nodes[p].style && !strcmp(CD.nodes[p].style, "font-weight:bold"), "inline style is kept");
    CHECK(CD.css && CD.css_len > 0, "<style> content is KEPT, not discarded");
    CHECK(!strncmp(CD.css, "p{color:red}", 12), "...and it is the stylesheet text");

    cparse("<script>var x = '<p>not markup</p>';</script><p>real</p>");
    CHECK(CD.css == 0, "<script> is still discarded -- nothing can run it");
    CHECK(find_tag("p") >= 0 && CD.nodes[find_tag("p")].first_child >= 0,
          "and script content never becomes elements");
}

static void t13_declarations(void) {
    printf("T13 declarations mean what they say:\n");
    struct vstyle v;
    memset(&v, 0, sizeof v);
    css_apply_decls("color:#c00; font-weight:bold; font-style:italic", strlen("color:#c00; font-weight:bold; font-style:italic"), &v);
    CHECK(v.color == 0xFFCC0000u, "#c00 expands to #cc0000");
    CHECK(v.bold == 1 && v.italic == 1, "weight and style applied");

    memset(&v, 0, sizeof v);
    css_apply_decls("color:red", strlen("color:red"), &v);
    CHECK(v.color == 0xFFFF0000u, "named colours work");

    memset(&v, 0, sizeof v);
    css_apply_decls("display:none", strlen("display:none"), &v);
    CHECK(v.display == VD_NONE, "display:none hides");

    memset(&v, 0, sizeof v);
    css_apply_decls("margin: 4px 8px 12px", strlen("margin: 4px 8px 12px"), &v);
    CHECK(v.margin_top == 4 && v.margin_bottom == 12, "the margin shorthand's 3-value form");

    memset(&v, 0, sizeof v);
    css_apply_decls("font-family: Menlo, monospace", strlen("font-family: Menlo, monospace"), &v);
    CHECK(v.mono == 1, "a mono family is recognised");

    memset(&v, 0, sizeof v);
    v.bold = 1;
    int n = css_apply_decls("float:left; -webkit-hack:1; color:", strlen("float:left; -webkit-hack:1; color:"), &v);
    CHECK(n == 0, "properties we cannot honour are skipped, not faked");
    CHECK(v.bold == 1, "...and skipping one does not disturb the rest");

    memset(&v, 0, sizeof v);
    css_apply_decls("color:red;;; ; font-weight:bold", strlen("color:red;;; ; font-weight:bold"), &v);
    CHECK(v.color && v.bold, "malformed separators do not derail the block");
}

static void t14_selectors(void) {
    printf("T14 selectors match and carry specificity:\n");
    cparse("<nav><ul><li class='item'><a id='home' href='#'>x</a></li></ul></nav>");
    int a = find_tag("a"), li = find_tag("li");
    struct css_sel s;

    CHECK(css_sel_parse("a", strlen("a"), &s) == 0 && css_sel_match(&s, &CD, a), "type selector");
    CHECK(s.spec == 1, "...specificity 1");
    CHECK(css_sel_parse(".item", strlen(".item"), &s) == 0 && css_sel_match(&s, &CD, li), "class selector");
    CHECK(s.spec == 10, "...specificity 10");
    CHECK(css_sel_parse("#home", strlen("#home"), &s) == 0 && css_sel_match(&s, &CD, a), "id selector");
    CHECK(s.spec == 100, "...specificity 100");
    CHECK(css_sel_parse("*", strlen("*"), &s) == 0 && css_sel_match(&s, &CD, a), "universal matches anything");

    CHECK(css_sel_parse("nav a", strlen("nav a"), &s) == 0 && css_sel_match(&s, &CD, a),
          "descendant selector crosses generations");
    CHECK(css_sel_parse("nav ul li a", strlen("nav ul li a"), &s) == 0 && css_sel_match(&s, &CD, a),
          "a four-part descendant chain");
    CHECK(css_sel_parse("li.item", strlen("li.item"), &s) == 0 && css_sel_match(&s, &CD, li),
          "a compound (type + class)");
    CHECK(css_sel_parse("p a", strlen("p a"), &s) == 0 && !css_sel_match(&s, &CD, a),
          "a wrong ancestor does NOT match");
    CHECK(css_sel_parse("li.missing", strlen("li.missing"), &s) == 0 && !css_sel_match(&s, &CD, li),
          "a wrong class does NOT match");
    CHECK(css_sel_parse("a:hover", strlen("a:hover"), &s) == 0 && css_sel_match(&s, &CD, a),
          "an unevaluable pseudo-class keeps the rest of the compound");
}

static void t15_cascade(void) {
    printf("T15 the cascade resolves by specificity, then order:\n");
    struct css_sheet sh;
    struct vstyle v;

    /* specificity beats document order */
    static const char *css1 = "p { color: red } .lead { color: blue }";
    cparse("<p class='lead'>x</p>");
    css_sheet_parse(&sh, css1, strlen(css1));
    CHECK(sh.n == 2, "two rules parsed");
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu, "the class (10) beats the type (1) whatever the order");

    /* ...and when specificity ties, the LATER rule wins */
    static const char *css2 = "p { color: red } p { color: blue }";
    css_sheet_parse(&sh, css2, strlen(css2));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu, "equal specificity -> document order decides");

    /* a weaker rule still contributes properties the stronger one omits */
    static const char *css3 = "p { color: red; font-style: italic } .lead { color: blue }";
    css_sheet_parse(&sh, css3, strlen(css3));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu && v.italic == 1,
          "the loser still supplies what the winner did not set");

    /* selector lists split into rules with their OWN specificity */
    static const char *css4 = "h1, .lead, #x { font-weight: bold }";
    css_sheet_parse(&sh, css4, strlen(css4));
    CHECK(sh.n == 3, "a comma list becomes three rules");
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.bold == 1, "...and the one that matches applies");

    /* comments and @media */
    static const char *css5 = "/* c */ p { color: red } @media print { p { color: lime } }";
    css_sheet_parse(&sh, css5, strlen(css5));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFFFF0000u, "comments skipped; @media dropped rather than misapplied");
}

static void t16_origin_order(void) {
    printf("T16 origin order: user-agent < author < inline:\n");
    struct css_sheet sh;
    struct vstyle root, v;
    vstyle_root(&root);

    cparse("<h1 style='color:lime'>t</h1>");
    static const char *css = "h1 { color: red }";
    css_sheet_parse(&sh, css, strlen(css));

    int h = find_tag("h1");
    vstyle_for_node(&CD, h, &root, &sh, &v);
    CHECK(v.color == 0xFF00FF00u, "inline style beats the author stylesheet");
    CHECK(v.size == 3 && v.bold == 1, "...and the UA stylesheet still supplies the rest");

    /* author beats UA */
    cparse("<h1>t</h1>");
    static const char *css6 = "h1 { font-weight: normal }";
    css_sheet_parse(&sh, css6, strlen(css6));
    vstyle_for_node(&CD, find_tag("h1"), &root, &sh, &v);
    CHECK(v.bold == 0, "the author can un-bold what the UA stylesheet bolded");

    /* no sheet at all == the pre-CSS behaviour, exactly */
    vstyle_for_node(&CD, find_tag("h1"), &root, 0, &v);
    CHECK(v.bold == 1 && v.size == 3, "no stylesheet -> the UA result, unchanged");
}

static void t17_bounded(void) {
    printf("T17 a stylesheet from a stranger is bounded:\n");
    static char big[40000];
    size_t k = 0;
    for (int i = 0; i < 400 && k < sizeof big - 40; i++)
        k += (size_t)snprintf(big + k, sizeof big - k, ".c%d { color: red } ", i);
    struct css_sheet sh;
    css_sheet_parse(&sh, big, k);
    CHECK(sh.n <= CSS_MAX_RULES, "never exceeds the rule table");
    CHECK(sh.truncated == 1, "and truncation is REPORTED, not silent");
}

int main(void) {
    printf("=== html-test ===\n");
    t1_structure(); t2_implicit_close(); t3_void_and_attrs(); t4_entities();
    t5_script_style(); t6_whitespace(); t7_malformed(); t8_urls(); t9_bounded();
    t10_url_parse(); t11_url_resolve();
    t12_attrs_and_style_block(); t13_declarations(); t14_selectors();
    t15_cascade(); t16_origin_order(); t17_bounded();
    printf("=== html-test: %s (%d failures) ===\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
