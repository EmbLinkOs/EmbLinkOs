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

static JSRuntime *g_rt;
static JSContext *g_ctx;
static struct html_doc *g_doc;
static const struct css_sheet *g_sheet;
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
static unsigned long long g_now;   /* last time the app pumped */

/* defined with the rest of the event machinery, below jsdom_open's globals */
static JSValue js_set_timeout(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_set_interval(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue js_clear_timer(JSContext *, JSValueConst, int, JSValueConst *);

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

static const JSCFunctionListEntry elem_proto[] = {
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
    }
    if (g_ctx) { JS_FreeContext(g_ctx); g_ctx = 0; }
    if (g_rt)  { JS_FreeRuntime(g_rt);  g_rt = 0; }
    g_doc = 0; g_sheet = 0; g_dirty = 0;
}

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
    JS_SetPropertyStr(g_ctx, g, "document", d);

    JS_SetPropertyStr(g_ctx, g, "setTimeout",
                      JS_NewCFunction(g_ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(g_ctx, g, "setInterval",
                      JS_NewCFunction(g_ctx, js_set_interval, "setInterval", 2));
    JS_SetPropertyStr(g_ctx, g, "clearTimeout",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(g_ctx, g, "clearInterval",
                      JS_NewCFunction(g_ctx, js_clear_timer, "clearInterval", 1));

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

int jsdom_pump_timers(unsigned long long now_ms) {
    if (!g_ctx) return 0;
    g_now = now_ms;
    int ran = 0;
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used || now_ms < g_timer[k].due) continue;
        JSValue fn = g_timer[k].fn;
        if (g_timer[k].repeat) {
            g_timer[k].due = now_ms + g_timer[k].every;
        } else {
            g_timer[k].used = 0;               /* one-shot: retire before the
                                                * call, so a handler that sets
                                                * a new timer gets a free slot */
        }
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
        ran = 1;
    }
    return ran;
}

unsigned long long jsdom_next_timer(void) {
    unsigned long long best = 0;
    for (int k = 0; k < MAX_TIMERS; k++) {
        if (!g_timer[k].used) continue;
        if (!best || g_timer[k].due < best) best = g_timer[k].due;
    }
    return best;
}
