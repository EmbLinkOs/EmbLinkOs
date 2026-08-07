/* user/web/form.c -- see form.h. */
#include <string.h>
#include <stdio.h>

#include "html.h"
#include "url.h"
#include "form.h"

static struct {
    int  used, node;
    char value[FORM_VALUE_MAX];
} g_field[FORM_MAX_FIELDS];

void form_reset(void) { memset(g_field, 0, sizeof g_field); }

static int slot_of(int node, int create) {
    for (int i = 0; i < FORM_MAX_FIELDS; i++)
        if (g_field[i].used && g_field[i].node == node) return i;
    if (!create) return -1;
    for (int i = 0; i < FORM_MAX_FIELDS; i++) {
        if (g_field[i].used) continue;
        g_field[i].used = 1; g_field[i].node = node; g_field[i].value[0] = 0;
        return i;
    }
    return -1;
}

char *form_value(struct html_doc *d, int node) {
    int existed = slot_of(node, 0) >= 0;
    int i = slot_of(node, 1);
    if (i < 0) return 0;
    /* Seed from the markup's `value=` ONCE, on first use. Re-seeding every
     * frame would fight the keyboard: the user types, the next render puts the
     * server's value back, and the field appears to reject input. */
    if (!existed && d && node >= 0 && node < d->n && d->nodes[node].value)
        snprintf(g_field[i].value, FORM_VALUE_MAX, "%s", d->nodes[node].value);
    return g_field[i].value;
}

const char *form_peek(int node) {
    int i = slot_of(node, 0);
    return i >= 0 ? g_field[i].value : "";
}

int form_set(struct html_doc *d, int node, const char *v) {
    char *b = form_value(d, node);
    if (!b || !v) return -1;
    snprintf(b, FORM_VALUE_MAX, "%s", v);
    return 0;
}

/* --- submission ---------------------------------------------------------- */

/* percent-encode into `out`; returns bytes written */
static size_t urlenc(const char *s, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        if (safe) { if (n + 1 < cap) out[n++] = (char)c; }
        else if (c == ' ') { if (n + 1 < cap) out[n++] = '+'; }
        else if (n + 3 < cap) {
            out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
        }
    }
    if (n < cap) out[n] = 0;
    return n;
}

/* the enclosing <form>, or -1 */
static int form_of(struct html_doc *d, int node) {
    for (int p = node; p >= 0; p = d->nodes[p].parent)
        if (d->nodes[p].kind == HTML_ELEM && !strcmp(d->nodes[p].tag, "form")) return p;
    return -1;
}

/* Append every NAMED control in this subtree. Unnamed controls are skipped,
 * which is the HTML rule and not an omission: a control with no name has
 * nothing to be called on the other side. */
static void collect(struct html_doc *d, int n, char *out, size_t cap, size_t *len) {
    if (n < 0 || n >= d->n) return;
    if (d->nodes[n].kind == HTML_ELEM) {
        const char *tag = d->nodes[n].tag;
        int is_ctl = !strcmp(tag, "input") || !strcmp(tag, "textarea") ||
                     !strcmp(tag, "select");
        const char *name = d->nodes[n].name;
        if (is_ctl && name && name[0]) {
            const char *v = form_peek(n);
            if (*len && *len + 1 < cap) out[(*len)++] = '&';
            *len += urlenc(name, out + *len, cap - *len);
            if (*len + 1 < cap) out[(*len)++] = '=';
            *len += urlenc(v, out + *len, cap - *len);
        }
    }
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling)
        collect(d, c, out, cap, len);
}

int form_submit(struct html_doc *d, int node, const char *base,
                char *url, size_t url_cap, char *body, size_t body_cap) {
    if (!d) return 0;
    int f = form_of(d, node);
    if (f < 0) return 0;

    static char qs[2048];
    size_t qn = 0;
    qs[0] = 0;
    collect(d, f, qs, sizeof qs, &qn);
    qs[qn < sizeof qs ? qn : sizeof qs - 1] = 0;

    /* action, resolved like any other link; absent action means THIS page,
     * which is what a search box on a page without one expects */
    const char *action = d->nodes[f].href;
    char target[512];
    if (action && action[0]) {
        if (url_resolve(base ? base : "", action, target, sizeof target) != 0)
            snprintf(target, sizeof target, "%s", action);
    } else {
        snprintf(target, sizeof target, "%s", base ? base : "");
    }

    /* a <form>'s method is parsed into `type` -- see html.h on why that slot
     * is safe to share and class/id are not */
    const char *method = d->nodes[f].type;
    int post = method && (method[0] == 'p' || method[0] == 'P');

    if (post) {
        snprintf(url, url_cap, "%s", target);
        snprintf(body, body_cap, "%s", qs);
        return 2;
    }
    /* GET: the query REPLACES any the action already carried, which is what a
     * browser does -- a form does not append to its own action's query. */
    char clean[512];
    snprintf(clean, sizeof clean, "%s", target);
    char *q = strchr(clean, '?');
    if (q) *q = 0;
    if (qn) snprintf(url, url_cap, "%s?%s", clean, qs);
    else    snprintf(url, url_cap, "%s", clean);
    if (body_cap) body[0] = 0;
    return 1;
}
