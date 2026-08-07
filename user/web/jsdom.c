/* user/web/jsdom.c -- see jsdom.h.
 *
 * An element is exposed to JavaScript as a plain object carrying its NODE
 * INDEX, not a pointer. The DOM arena is an array that a mutation can grow, so
 * a pointer handed to a script is a pointer that a later appendChild could
 * invalidate -- and a script holding a stale element is a use-after-free with
 * a user's page as the trigger. An index cannot dangle; the worst it can do is
 * refer to a node that no longer exists, which every accessor checks.
 *
 * querySelector is nearly free here, and that is the payoff for having split
 * the CSS engine by concern: sel.c already answers "does this selector match
 * this element", so the binding is a walk plus a call.
 */
#include <string.h>
#include <stdio.h>

#include "quickjs.h"
#include "html.h"
#include "style.h"
#include "css.h"
#include "jsdom.h"
#include "fetchjob.h"
#include "net.h"
#include "form.h"
#include "url.h"

static JSRuntime *g_rt;
static JSContext *g_ctx;
static struct html_doc *g_doc;
static const struct css_sheet *g_sheet;
static const char *g_url;      /* the page's own address, for location */
static int  g_dirty;
static void (*g_console)(const char *line);

/* ---- listeners + timers -------------------------------------------------
 * Fixed tables, like everything else that a stranger's page can grow. A page
 * that registers more than this gets the first N and keeps working, which is
 * the same bargain the DOM arena and the CSS rule table make.
 */
#define MAX_LISTENERS 64
#define MAX_TIMERS    32

static struct { int node; JSValue fn; int used; } g_listen[MAX_LISTENERS];
static struct {
    int      id, used, repeat;
    unsigned long long due, every;
    JSValue  fn;
} g_timer[MAX_TIMERS];
static int g_timer_seq = 1;

/* ---- fetch --------------------------------------------------------------
 * A page's fetch shares the ONE worker the document and the images use, and
 * takes its turn behind them: the page you asked for outranks the data a
 * script wants about it. FETCH_TAG is how the poll knows the result is ours
 * (fetchjob.h -- a poll that does not own a result must not consume it).
 */
#define FETCH_TAG  3
#define FETCH_MAX  4
#define FETCH_BUF  (256 * 1024)

static struct {
    int     used, started;
    char    url[512];
    JSValue resolve, reject;
} g_fetch[FETCH_MAX];
static char g_fetch_buf[FETCH_BUF];
static int  g_fetch_active = -1;
static unsigned long long g_now;   /* last time the app pumped */

/* defined with the rest of the event machinery, below jsdom_open's globals */
static JSValue js_set_timeout(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_set_interval(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_clear_timer(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_fetch(JSContext *, JSValueConst, int, JSValueConst *);

/* ---- element objects ---------------------------------------------------- */

static JSClassID g_elem_class;

/* The node index lives on the object; see the file header for why it is an
 * index and not a pointer. */
static int elem_index(JSContext *ctx, JSValueConst v) {
    JSValue p = JS_GetPropertyStr(ctx, v, "__i");
    int32_t i = -1;
    if (!JS_IsUndefined(p)) JS_ToInt32(ctx, &i, p);
    JS_FreeValue(ctx, p);
    if (!g_doc || i < 0 || i >= g_doc->n) return -1;
    return i;
}

static JSValue make_elem(JSContext *ctx, int idx);

/* textContent: reading concatenates the subtree's text, which is what the DOM
 * specifies and what a script asking for "the words in this element" means. */
static void gather_text(struct html_doc *d, int n, char *out, size_t cap, size_t *len) {
    if (n < 0 || n >= d->n) return;
    if (d->nodes[n].kind == HTML_TEXT) {
        const char *t = d->nodes[n].text;
        if (t) { size_t l = strlen(t);
                 if (*len + l < cap) { memcpy(out + *len, t, l); *len += l; } }
        return;
    }
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling)
        gather_text(d, c, out, cap, len);
}

static JSValue elem_get_text(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    static char buf[8192];
    size_t len = 0;
    gather_text(g_doc, i, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    return JS_NewString(ctx, buf);
}

static JSValue elem_set_text(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (html_set_text(g_doc, i, s) == 0) g_dirty = 1;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue elem_get_attr(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const struct html_node *e = &g_doc->nodes[i];
    const char *v = 0;
    if      (!strcmp(name, "href") || !strcmp(name, "src")) v = e->href;
    else if (!strcmp(name, "class"))  v = e->klass;
    else if (!strcmp(name, "id"))     v = e->id;
    else if (!strcmp(name, "alt"))    v = e->alt;
    else if (!strcmp(name, "style"))  v = e->style;
    JS_FreeCString(ctx, name);
    /* The attributes this parser keeps are the ones something downstream can
     * ACT on (html.h §3). Anything else is genuinely absent, and null says so
     * rather than pretending the document did not have it. */
    return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue elem_get_tag(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    return JS_NewString(ctx, g_doc->nodes[i].tag);
}

/* Set an inline style declaration, e.g. el.setStyle("color:red"). Not the DOM's
 * `el.style.color = 'red'` -- that needs a property-per-CSS-property proxy
 * object, which is a lot of surface for the same effect. Named differently
 * BECAUSE it is different: a script author must not think they have the real
 * one and then wonder why `el.style.color` reads back undefined. */
static JSValue elem_set_style(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    /* borrow the document's own arena, exactly as the parser does */
    char *held = html_intern(g_doc, s, strlen(s));
    if (held) { g_doc->nodes[i].style = held; g_dirty = 1; }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue elem_add_listener(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 2) return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;
    /* Only 'click' is real here. An event name we cannot deliver is REFUSED
     * loudly rather than registered and silently never fired -- a listener
     * that never runs is the hardest kind of bug to see. */
    if (strcmp(type, "click") != 0) {
        JS_FreeCString(ctx, type);
        return JS_ThrowTypeError(ctx, "only 'click' is supported by this browser");
    }
    JS_FreeCString(ctx, type);
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "listener must be a function");

    for (int k = 0; k < MAX_LISTENERS; k++) {
        if (g_listen[k].used) continue;
        g_listen[k].used = 1;
        g_listen[k].node = i;
        g_listen[k].fn = JS_DupValue(ctx, argv[1]);
        return JS_UNDEFINED;
    }
    return JS_ThrowInternalError(ctx, "too many listeners");
}

/* A control's value is the USER's, not the document's -- form.c holds it (see
 * form.h). Exposing it as a property is what lets a script validate, prefill
 * or clear a field, which is most of what page scripts do with forms. */
static JSValue elem_get_value(JSContext *ctx, JSValueConst this_val) {
    int i = elem_index(ctx, this_val);
    if (i < 0) return JS_UNDEFINED;
    return JS_NewString(ctx, form_peek(i));
}
static JSValue elem_set_value(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    int i = elem_index(ctx, this_val);
    if (i < 0 || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    form_set(g_doc, i, s);
    g_dirty = 1;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry elem_proto[] = {
    JS_CGETSET_DEF("value", elem_get_value, 0),
    JS_CFUNC_DEF("setValue", 1, elem_set_value),
    JS_CFUNC_DEF("addEventListener", 2, elem_add_listener),
    JS_CGETSET_DEF("textContent", elem_get_text, 0),
    JS_CGETSET_DEF("tagName", elem_get_tag, 0),
    JS_CFUNC_DEF("setText", 1, elem_set_text),
    JS_CFUNC_DEF("getAttribute", 1, elem_get_attr),
    JS_CFUNC_DEF("setStyle", 1, elem_set_style),
};

static JSValue make_elem(JSContext *ctx, int idx) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__i", JS_NewInt32(ctx, idx));
    JS_SetPropertyFunctionList(ctx, o, elem_proto,
                               sizeof elem_proto / sizeof elem_proto[0]);
    return o;
}

/* ---- document ----------------------------------------------------------- */

/* Depth-first, document order -- the order querySelectorAll must return, and
 * the order a reader would find them in. */
static int find_match(struct html_doc *d, int n, const struct css_sel *sel,
                      int *out, int max, int *count) {
    if (n < 0 || n >= d->n) return 0;
    if (d->nodes[n].kind == HTML_ELEM && css_sel_match(sel, d, n)) {
        if (out && *count < max) out[*count] = n;
        (*count)++;
        if (!out) return n;                      /* querySelector: first wins */
    }
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        int r = find_match(d, c, sel, out, max, count);
        if (!out && r >= 0) return r;
    }
    return -1;
}

static JSValue doc_query(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !g_doc) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    struct css_sel sel;
    int ok = css_sel_parse(s, strlen(s), &sel) == 0;
    JS_FreeCString(ctx, s);
    if (!ok) return JS_NULL;
    int count = 0;
    int hit = find_match(g_doc, g_doc->root, &sel, 0, 0, &count);
    return hit >= 0 ? make_elem(ctx, hit) : JS_NULL;
}

static JSValue doc_query_all(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    if (argc < 1 || !g_doc) return arr;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return arr;
    struct css_sel sel;
    int ok = css_sel_parse(s, strlen(s), &sel) == 0;
    JS_FreeCString(ctx, s);
    if (!ok) return arr;
    static int hits[256];
    int count = 0;
    find_match(g_doc, g_doc->root, &sel, hits, 256, &count);
    if (count > 256) count = 256;
    for (int i = 0; i < count; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, make_elem(ctx, hits[i]));
    return arr;
}

static JSValue doc_get_title(JSContext *ctx, JSValueConst this_val) {
    (void)this_val;
    if (!g_doc) return JS_UNDEFINED;
    struct css_sel sel;
    if (css_sel_parse("title", 5, &sel) != 0) return JS_UNDEFINED;
    int count = 0;
    int t = find_match(g_doc, g_doc->root, &sel, 0, 0, &count);
    if (t < 0) return JS_NewString(ctx, "");
    static char buf[512]; size_t len = 0;
    gather_text(g_doc, t, buf, sizeof buf - 1, &len);
    buf[len] = 0;
    return JS_NewString(ctx, buf);
}

/* ---- console ------------------------------------------------------------ */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    static char line[1024];
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        int w = snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", s);
        JS_FreeCString(ctx, s);
        if (w > 0) n += (size_t)w;
        if (n >= sizeof line - 1) break;
    }
    line[n < sizeof line ? n : sizeof line - 1] = 0;
    if (g_console) g_console(line);
    return JS_UNDEFINED;
}

/* ---- lifecycle ---------------------------------------------------------- */

void jsdom_set_console(void (*fn)(const char *line)) { g_console = fn; }
int  jsdom_take_dirty(void) { int d = g_dirty; g_dirty = 0; return d; }

void jsdom_close(void) {
    if (g_ctx) {
        for (int k = 0; k < MAX_LISTENERS; k++)
            if (g_listen[k].used) { JS_FreeValue(g_ctx, g_listen[k].fn); g_listen[k].used = 0; }
        for (int k = 0; k < MAX_TIMERS; k++)
            if (g_timer[k].used) { JS_FreeValue(g_ctx, g_timer[k].fn); g_timer[k].used = 0; }
        /* An unsettled promise whose page is gone is not an error to report --
         * there is no longer anyone to report it to. Drop the handlers. */
        for (int k = 0; k < FETCH_MAX; k++)
            if (g_fetch[k].used) {
                JS_FreeValue(g_ctx, g_fetch[k].resolve);
                JS_FreeValue(g_ctx, g_fetch[k].reject);
                g_fetch[k].used = 0;
            }
        g_fetch_active = -1;
    }
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = 0; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt = 0; }
    g_doc = 0; g_sheet = 0; g_dirty = 0;
}

void jsdom_set_url(const char *url) { g_url = url; }

int jsdom_open(struct html_doc *doc, const struct css_sheet *sheet) {
    jsdom_close();
    g_doc = doc; g_sheet = sheet;
    g_rt = JS_NewRuntime();
    if (!g_rt) return -1;
    /* A page's script gets a BUDGET. An accidental `while(1)` on a stranger's
     * page must not take the window with it, and a browser that can be hung by
     * one line of someone else's JavaScript is not one you can browse with. */
    JS_SetMemoryLimit(g_rt, 16u * 1024 * 1024);
    JS_SetMaxStackSize(g_rt, 512u * 1024);
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) { JS_FreeRuntime(g_rt); g_rt = 0; return -1; }

    JSValue g = JS_GetGlobalObject(g_ctx);

    JSValue console = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, console, "log",
                      JS_NewCFunction(g_ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(g_ctx, g, "console", console);

    JSValue d = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, d, "querySelector",
                      JS_NewCFunction(g_ctx, doc_query, "querySelector", 1));
    JS_SetPropertyStr(g_ctx, d, "querySelectorAll",
                      JS_NewCFunction(g_ctx, doc_query_all, "querySelectorAll", 1));
    JSAtom t = JS_NewAtom(g_ctx, "title");
    JS_DefinePropertyGetSet(g_ctx, d, t,
                            JS_NewCFunction(g_ctx, (JSCFunction *)doc_get_title, "title", 0),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(g_ctx, t);
    /* location: a page that cannot read its own URL cannot act on a form's
     * query string, which is how half the web's search results pages work. */
    JSValue loc = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, loc, "href", JS_NewString(g_ctx, g_url ? g_url : ""));
    {
        struct url u;
        if (g_url && url_parse(g_url, &u) == 0) {
            JS_SetPropertyStr(g_ctx, loc, "pathname", JS_NewString(g_ctx, u.path));
            char q[300];
            snprintf(q, sizeof q, "%s%s", u.query[0] ? "?" : "", u.query);
            JS_SetPropertyStr(g_ctx, loc, "search", JS_NewString(g_ctx, q));
        } else {
            JS_SetPropertyStr(g_ctx, loc, "pathname", JS_NewString(g_ctx, ""));
            JS_SetPropertyStr(g_ctx, loc, "search", JS_NewString(g_ctx, ""));
        }
    }
    JS_SetPropertyStr(g_ctx, g, "location", loc);

    JS_SetPropertyStr(g_ctx, g, "document", d);

    JS_SetPropertyStr(g_ctx, g, "setTimeout",
                      JS_NewCFunction(g_ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(g_ctx, g, "setInterval",
                      JS_NewCFunction(g_ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(g_ctx, g, "clearTimeout",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(g_ctx, g, "clearInterval",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearInterval", 1));
    JS_SetPropertyStr(g_ctx, g, "fetch",
                      JS_NewCFunction(g_ctx, js_fetch, "fetch", 1));

    JS_FreeValue(g_ctx, g);
    (void)g_elem_class;
    return 0;
}

static int eval_one(const char *src, size_t len, const char *name) {
    JSValue v = JS_Eval(g_ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        if (g_console && m) { char b[512]; snprintf(b, sizeof b, "%s", m); g_console(b); }
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        JS_FreeValue(g_ctx, v);
        return -1;
    }
    JS_FreeValue(g_ctx, v);
    return 0;
}

int jsdom_eval(const char *src, const char *name) {
    if (!g_ctx || !src) return -1;
    return eval_one(src, strlen(src), name ? name : "<eval>");
}

int jsdom_run_scripts(void) {
    if (!g_ctx || !g_doc) return 0;
    int failed = 0;
    for (int i = 0; i < g_doc->n_js; i++) {
        char name[24];
        snprintf(name, sizeof name, "<script %d>", i + 1);
        /* One script throwing must not stop the next: a page's scripts are
         * independent, and in a browser a broken third-party tag does not
         * blank the document. */
        if (eval_one(g_doc->js[i], g_doc->js_len[i], name) != 0) failed++;
    }
    return failed;
}

/* ---- events ------------------------------------------------------------- */

int jsdom_has_listener(int node) {
    for (int k = 0; k < MAX_LISTENERS; k++)
        if (g_listen[k].used && g_listen[k].node == node) return 1;
    return 0;
}

int jsdom_dispatch_click(int node) {
    if (!g_ctx) return 0;
    int ran = 0;
    for (int k = 0; k < MAX_LISTENERS; k++) {
        if (!g_listen[k].used || g_listen[k].node != node) continue;
        JSValue ev = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, ev, "type", JS_NewString(g_ctx, "click"));
        JS_SetPropertyStr(g_ctx, ev, "target", make_elem(g_ctx, node));
        JSValue argv[1] = { ev };
        JSValue r = JS_Call(g_ctx, g_listen[k].fn, JS_UNDEFINED, 1, argv);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(g_ctx);
            const char *m = JS_ToCString(g_ctx, e);
            if (g_console && m) g_console(m);
            if (m) JS_FreeCString(g_ctx, m);
            JS_FreeValue(g_ctx, e);
        }
        JS_FreeValue(g_ctx, r);
        JS_FreeValue(g_ctx, ev);
        ran = 1;
    }
    return ran;
}

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int repeat) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    if (ms < 0) ms = 0;
    /* A floor on repeats. setInterval(f, 0) is a page asking to be run as fast
     * as the machine can go, which on a shared UI thread means the window
     * stops responding -- and the page cannot tell that it did anything wrong.
     * Clamping is kinder than freezing. */
    if (repeat && ms < 10) ms = 10;

    for (int k = 0; k < MAX_TIMERS; k++) {
        if (g_timer[k].used) continue;
        g_timer[k].used = 1;
        g_timer[k].repeat = repeat;
        g_timer[k].every = (unsigned long long)ms;
        g_timer[k].due = g_now + (unsigned long long)ms;
        g_timer[k].fn = JS_DupValue(ctx, argv[0]);
        g_timer[k].id = g_timer_seq++;
        return JS_NewInt32(ctx, g_timer[k].id);
    }
    return JS_ThrowInternalError(ctx, "too many timers");
}
static JSValue js_set_timeout(JSContext *c, JSValueConst t, int n, JSValueConst *a)
{ return js_set_timer(c, t, n, a, 0); }
static JSValue js_set_interval(JSContext *c, JSValueConst t, int n, JSValueConst *a)
{ return js_set_timer(c, t, n, a, 1); }

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    for (int k = 0; k < MAX_TIMERS; k++)
        if (g_timer[k].used && g_timer[k].id == id) {
            JS_FreeValue(ctx, g_timer[k].fn);
            g_timer[k].used = 0;
        }
    return JS_UNDEFINED;
}

/* fetch(url) -> Promise. The Promise is real: QuickJS hands back its resolve
 * and reject functions, we stash them on a slot, and the pump settles the
 * Promise when the bytes land -- which is what makes `await fetch(...)` and
 * `.then(...)` behave as a script author expects rather than as a stub. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);   /* [resolve, reject] */
    if (JS_IsException(promise)) return promise;

    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : 0;
    int slot = -1;
    for (int k = 0; k < FETCH_MAX; k++) if (!g_fetch[k].used) { slot = k; break; }

    if (!url || slot < 0) {
        /* Reject NOW, on this stack. The pump would work too, but a fetch that
         * cannot even be queued should fail on the same turn it was asked, the
         * way a browser rejects a malformed URL immediately. */
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, url ? "too many concurrent fetches" : "fetch: no URL"));
        JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst[]){ err });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
        if (url) JS_FreeCString(ctx, url);
        return promise;
    }

    g_fetch[slot].used = 1;
    g_fetch[slot].started = 0;
    snprintf(g_fetch[slot].url, sizeof g_fetch[slot].url, "%s", url);
    g_fetch[slot].resolve = funcs[0];
    g_fetch[slot].reject  = funcs[1];
    JS_FreeCString(ctx, url);
    return promise;
}

/* Settle a fetch slot: build a small Response-shaped object ({ ok, status,
 * text() }) and resolve, or reject with the network error. Deliberately not
 * the whole Fetch spec -- headers, streaming, a real Body -- because a
 * documentation page's script wants status and text, and a binding that
 * pretends to more than it has is the lie this browser refuses. */
static void fetch_settle(int slot, const struct vnet_result *res) {
    JSContext *ctx = g_ctx;
    JSValue rf = g_fetch[slot].resolve, jf = g_fetch[slot].reject;
    g_fetch[slot].used = 0;

    if (res->err[0] && res->len == 0) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, res->err));
        JSValue r = JS_Call(ctx, jf, JS_UNDEFINED, 1, (JSValueConst[]){ err });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
    } else {
        JSValue body = JS_NewStringLen(ctx, g_fetch_buf, res->len);
        JSValue resp = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, res->status / 100 == 2));
        JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, res->status));
        /* text() returns a resolved Promise of the body -- Response.text() is
         * async in the real API, and matching that means `await resp.text()`
         * is what a script writes here too. */
        JSValue textfn = JS_Eval(ctx, "(function(b){return function(){return Promise.resolve(b);};})",
                                 60, "<fetch>", JS_EVAL_TYPE_GLOBAL);
        JSValue bound = JS_Call(ctx, textfn, JS_UNDEFINED, 1, (JSValueConst[]){ body });
        JS_SetPropertyStr(ctx, resp, "text", bound);
        JS_FreeValue(ctx, textfn);
        JSValue r = JS_Call(ctx, rf, JS_UNDEFINED, 1, (JSValueConst[]){ resp });
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, resp); JS_FreeValue(ctx, body);
    }
    JS_FreeValue(ctx, rf);
    JS_FreeValue(ctx, jf);
}

/* Move fetches along: reap the active one, start the next. One at a time on
 * the shared worker, behind the document and its images. */
static int fetch_pump(void) {
    int changed = 0;
    if (g_fetch_active >= 0) {
        struct vnet_result res;
        int r = fetchjob_poll(FETCH_TAG, &res);
        if (r == 1) {
            fetch_settle(g_fetch_active, &res);
            g_fetch_active = -1; changed = 1;
        } else if (r < 0) {                 /* the job vanished (page changed) */
            if (g_fetch_active < FETCH_MAX && g_fetch[g_fetch_active].used) {
                g_fetch[g_fetch_active].used = 0;
                JS_FreeValue(g_ctx, g_fetch[g_fetch_active].resolve);
                JS_FreeValue(g_ctx, g_fetch[g_fetch_active].reject);
            }
            g_fetch_active = -1; changed = 1;
        }
    }
    if (g_fetch_active < 0 && !fetchjob_busy()) {
        for (int k = 0; k < FETCH_MAX; k++) {
            if (!g_fetch[k].used || g_fetch[k].started) continue;
            if (fetchjob_start(g_fetch[k].url, g_fetch_buf, sizeof g_fetch_buf, FETCH_TAG) == 0) {
                g_fetch[k].started = 1;
                g_fetch_active = k;
                changed = 1;
            }
            break;
        }
    }
    return changed;
}

/* Drain QuickJS's pending-job queue: promise reactions, queueMicrotask, and
 * the async continuations `await` compiles to. This is the beating heart of
 * async in the engine, and forgetting it makes every promise a silent no-op. */
static int drain_jobs(void) {
    int did = 0;
    JSContext *c;
    for (;;) {
        int r = JS_ExecutePendingJob(g_rt, &c);
        if (r <= 0) {                        /* 0 = none left, <0 = a job threw */
            if (r < 0 && c) {
                JSValue e = JS_GetException(c);
                const char *m = JS_ToCString(c, e);
                if (g_console && m) g_console(m);
                if (m) JS_FreeCString(c, m);
                JS_FreeValue(c, e);
                did = 1;
                continue;                    /* keep draining past a rejection */
            }
            break;
        }
        did = 1;
    }
    return did;
}

int jsdom_pump(unsigned long long now_ms) {
    if (!g_ctx) return 0;
    g_now = now_ms;
    int changed = 0;
    if (fetch_pump()) changed = 1;

    /* timers */
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used || now_ms < g_timer[k].due) continue;
        JSValue fn = g_timer[k].fn;
        if (g_timer[k].repeat) g_timer[k].due = now_ms + g_timer[k].every;
        else                   g_timer[k].used = 0;
        JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(g_ctx);
            const char *m = JS_ToCString(g_ctx, e);
            if (g_console && m) g_console(m);
            if (m) JS_FreeCString(g_ctx, m);
            JS_FreeValue(g_ctx, e);
        }
        JS_FreeValue(g_ctx, r);
        if (!g_timer[k].repeat) JS_FreeValue(g_ctx, fn);
        changed = 1;
    }

    /* microtasks LAST: a timer or a settled fetch may have queued promise
     * reactions, and they must run this frame, not next. */
    if (drain_jobs()) changed = 1;
    return changed;
}

unsigned long long jsdom_next_timer(void) {
    unsigned long long best = 0;
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used) continue;
        if (!best || g_timer[k].due < best) best = g_timer[k].due;
    }
    return best;
}

int jsdom_busy(void) {
    for (int k = 0; k < FETCH_MAX; k++) if (g_fetch[k].used) return 1;
    return 0;
}
