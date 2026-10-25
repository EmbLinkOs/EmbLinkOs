/* display_arrange.c -- see display_arrange.h. */
#include "display_arrange.h"
#include "display/layout.h"
#include <string.h>

static unsigned outputs(const struct embk_display_layout *d) {
    return d->count < EMBK_DISPLAY_MAX_OUTPUTS ? d->count : EMBK_DISPLAY_MAX_OUTPUTS;
}

static int has_mode(const struct embk_output *o, uint32_t w, uint32_t h) {
    for (unsigned k = 0; k < o->mode_count && k < 16; k++)
        if (o->modes[k].width == w && o->modes[k].height == h) return 1;
    return 0;
}

static void preferred(const struct embk_output *o, uint32_t *w, uint32_t *h) {
    *w = *h = 0;
    for (unsigned k = 0; k < o->mode_count && k < 16; k++)
        if (o->modes[k].flags & EMBK_MONITOR_PREFERRED) { *w = o->modes[k].width; *h = o->modes[k].height; return; }
    if (o->mode_count) { *w = o->modes[0].width; *h = o->modes[0].height; }
}

static int primary_of(const struct arrange *a) {
    for (unsigned i = 0; i < a->count; i++) if (a->out[i].enabled && a->out[i].primary) return (int)i;
    return -1;
}

/* Describe every enabled output by its side of the primary, from positions. */
static void describe_sides(struct arrange *a, const int32_t *x, const int32_t *y) {
    int p = primary_of(a);
    if (p < 0) return;
    int64_t px = x[p], py = y[p], pr = px + a->out[p].width, pb = py + a->out[p].height;
    for (unsigned i = 0; i < a->count; i++) {
        struct arrange_out *o = &a->out[i];
        o->side = ARRANGE_RIGHT; o->order = 0; o->offset = 0;
        if ((int)i == p || !o->enabled || a->mirror) continue;
        int64_t r = (int64_t)x[i] + o->width, b = (int64_t)y[i] + o->height, order, offset;
        if (x[i] >= pr)      { o->side = ARRANGE_RIGHT; order = x[i] - pr; offset = y[i] - py; }
        else if (r <= px)    { o->side = ARRANGE_LEFT;  order = px - r;    offset = y[i] - py; }
        else if (b <= py)    { o->side = ARRANGE_ABOVE; order = py - b;    offset = x[i] - px; }
        else                 { o->side = ARRANGE_BELOW; order = y[i] - pb; offset = x[i] - px; }
        o->order = (int32_t)(order < 0 ? 0 : order);
        o->offset = (int32_t)offset;
    }
}

void arrange_from_layout(const struct embk_display_layout *d, struct arrange *a) {
    memset(a, 0, sizeof *a);
    a->count = outputs(d);
    a->mirror = (d->flags & EMBK_LAYOUT_MIRROR) != 0;
    int32_t x[EMBK_DISPLAY_MAX_OUTPUTS] = { 0 }, y[EMBK_DISPLAY_MAX_OUTPUTS] = { 0 };
    for (unsigned i = 0; i < a->count; i++) {
        const struct embk_output *o = &d->outputs[i];
        struct arrange_out *e = &a->out[i];
        e->enabled = (o->flags & EMBK_OUTPUT_ENABLED) && o->width && o->height;
        e->primary = e->enabled && (o->flags & EMBK_OUTPUT_PRIMARY);
        if (e->enabled) { e->width = o->width; e->height = o->height; }
        else preferred(o, &e->width, &e->height);
        x[i] = o->x; y[i] = o->y;
    }
    describe_sides(a, x, y);
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Coordinates for every enabled output: the primary at the origin, each side
 * packed outward in order, each display level with the one before it on that
 * side by its offset, as far as still sharing an edge with it allows. */
static void place(const struct arrange *a, int32_t *x, int32_t *y) {
    int p = primary_of(a);
    for (unsigned i = 0; i < a->count; i++) x[i] = y[i] = 0;
    if (p < 0 || a->mirror) return;
    for (int side = ARRANGE_RIGHT; side <= ARRANGE_BELOW; side++) {
        int done[EMBK_DISPLAY_MAX_OUTPUTS] = { 0 };
        int64_t px = 0, py = 0, pw = a->out[p].width, ph = a->out[p].height;   /* the one before */
        int64_t cursor = side == ARRANGE_RIGHT ? pw : side == ARRANGE_BELOW ? ph : 0;
        for (;;) {
            int next = -1;
            for (unsigned i = 0; i < a->count; i++) {
                const struct arrange_out *o = &a->out[i];
                if ((int)i == p || !o->enabled || o->side != side || done[i]) continue;
                if (next < 0 || o->order < a->out[next].order) next = (int)i;
            }
            if (next < 0) break;
            done[next] = 1;
            const struct arrange_out *o = &a->out[next];
            int64_t w = o->width, h = o->height, nx, ny;
            if (side == ARRANGE_RIGHT || side == ARRANGE_LEFT) {
                ny = clamp64(o->offset, py - h + 1, py + ph - 1);
                nx = side == ARRANGE_RIGHT ? cursor : cursor - w;
                cursor = side == ARRANGE_RIGHT ? cursor + w : cursor - w;
            } else {
                nx = clamp64(o->offset, px - w + 1, px + pw - 1);
                ny = side == ARRANGE_BELOW ? cursor : cursor - h;
                cursor = side == ARRANGE_BELOW ? cursor + h : cursor - h;
            }
            x[next] = (int32_t)nx; y[next] = (int32_t)ny;
            px = nx; py = ny; pw = w; ph = h;
        }
    }
}

/* Check a request against these outputs the way the kernel will, and put its
 * desktop at the origin. */
static int check(const struct embk_display_layout *d, struct embk_layout_request *r) {
    struct layout l;
    memset(&l, 0, sizeof l);
    l.count = outputs(d);
    l.mirror = (r->flags & EMBK_LAYOUT_MIRROR) != 0;
    for (unsigned i = 0; i < l.count; i++) {
        struct layout_out *o = &l.out[i];
        o->connected = (d->outputs[i].flags & EMBK_OUTPUT_CONNECTED) != 0;
        o->enabled = r->out[i].enabled != 0;
        o->primary = r->out[i].primary != 0;
        o->x = r->out[i].x; o->y = r->out[i].y; o->w = r->out[i].width; o->h = r->out[i].height;
        if (o->enabled && o->connected && !has_mode(&d->outputs[i], o->w, o->h)) return ARRANGE_MODE;
    }
    unsigned in_use = 0;
    for (unsigned i = 0; i < l.count; i++) in_use += l.out[i].enabled ? 1u : 0u;
    if (d->max_enabled && in_use > d->max_enabled) return ARRANGE_TOO_MANY;
    layout_normalize(&l);
    int v = layout_validate(&l, 0, 0);
    if (v != LAYOUT_OK) return v;          /* the ARRANGE_* codes are the LAYOUT_* ones */
    for (unsigned i = 0; i < l.count; i++) {
        if (!l.out[i].enabled) continue;
        r->out[i].x = l.out[i].x; r->out[i].y = l.out[i].y;
    }
    return ARRANGE_OK;
}

int arrange_request(const struct embk_display_layout *d, const struct arrange *a, struct embk_layout_request *r) {
    memset(r, 0, sizeof *r);
    r->version = EMBK_DISPLAY_LAYOUT_VERSION;
    r->generation = d->generation;
    r->flags = a->mirror ? EMBK_LAYOUT_MIRROR : 0;
    r->count = outputs(d);
    if (a->count != r->count) return ARRANGE_MISMATCH;
    int32_t x[EMBK_DISPLAY_MAX_OUTPUTS], y[EMBK_DISPLAY_MAX_OUTPUTS];
    place(a, x, y);
    for (unsigned i = 0; i < r->count; i++) {
        if (!a->out[i].enabled) continue;
        r->out[i].enabled = 1;
        r->out[i].primary = a->out[i].primary != 0;
        r->out[i].width = a->out[i].width; r->out[i].height = a->out[i].height;
        r->out[i].x = x[i]; r->out[i].y = y[i];
    }
    return check(d, r);
}

int arrange_differs(const struct embk_display_layout *d, const struct embk_layout_request *r) {
    if (!(d->flags & EMBK_LAYOUT_MIRROR) != !(r->flags & EMBK_LAYOUT_MIRROR)) return 1;
    for (unsigned i = 0; i < outputs(d); i++) {
        const struct embk_output *o = &d->outputs[i];
        int on = (o->flags & EMBK_OUTPUT_ENABLED) != 0;
        if (on != (r->out[i].enabled != 0)) return 1;
        if (!on) continue;
        if (!(o->flags & EMBK_OUTPUT_PRIMARY) != !r->out[i].primary || o->x != r->out[i].x || o->y != r->out[i].y ||
            o->width != r->out[i].width || o->height != r->out[i].height) return 1;
    }
    return 0;
}

int arrange_set_primary(const struct embk_display_layout *d, struct arrange *a, unsigned i) {
    if (i >= a->count || !a->out[i].enabled) return ARRANGE_PRIMARY;
    if (a->out[i].primary) return ARRANGE_OK;
    struct embk_layout_request r;
    int rc = arrange_request(d, a, &r);
    if (rc) return rc;
    int32_t x[EMBK_DISPLAY_MAX_OUTPUTS], y[EMBK_DISPLAY_MAX_OUTPUTS];
    for (unsigned k = 0; k < a->count; k++) { x[k] = r.out[k].x; y[k] = r.out[k].y; a->out[k].primary = k == i; }
    describe_sides(a, x, y);
    return ARRANGE_OK;
}

int arrange_set_enabled(const struct embk_display_layout *d, struct arrange *a, unsigned i, int enabled) {
    if (i >= a->count) return ARRANGE_DISCONNECTED;
    struct arrange_out *o = &a->out[i];
    if (!!enabled == !!o->enabled) return ARRANGE_OK;
    unsigned in_use = 0;
    for (unsigned k = 0; k < a->count; k++) in_use += a->out[k].enabled ? 1u : 0u;
    if (enabled) {
        if (!(d->outputs[i].flags & EMBK_OUTPUT_CONNECTED)) return ARRANGE_DISCONNECTED;
        if (d->max_enabled && in_use >= d->max_enabled) return ARRANGE_TOO_MANY;
        uint32_t w, h;
        int p = primary_of(a);
        if (a->mirror && p >= 0) {
            w = a->out[p].width; h = a->out[p].height;
            if (!has_mode(&d->outputs[i], w, h)) return ARRANGE_MIRROR_SIZE;
        } else preferred(&d->outputs[i], &w, &h);
        if (!w || !h) return ARRANGE_MODE;
        o->enabled = 1; o->primary = in_use == 0;
        o->width = w; o->height = h;
        o->side = ARRANGE_RIGHT; o->order = INT32_MAX; o->offset = 0;   /* outermost */
        return ARRANGE_OK;
    }
    if (in_use <= 1) return ARRANGE_NONE;
    if (!o->primary) { o->enabled = 0; return ARRANGE_OK; }
    /* The primary leaves: the first display still in use takes its place, and
     * the rest are described from where they are. */
    struct embk_layout_request r;
    int rc = arrange_request(d, a, &r);
    if (rc) return rc;
    int32_t x[EMBK_DISPLAY_MAX_OUTPUTS], y[EMBK_DISPLAY_MAX_OUTPUTS];
    for (unsigned k = 0; k < a->count; k++) { x[k] = r.out[k].x; y[k] = r.out[k].y; }
    o->enabled = 0; o->primary = 0;
    for (unsigned k = 0; k < a->count; k++) if (a->out[k].enabled) { a->out[k].primary = 1; break; }
    describe_sides(a, x, y);
    return ARRANGE_OK;
}

unsigned arrange_common_modes(const struct embk_display_layout *d, const struct arrange *a,
                              uint32_t *w, uint32_t *h, unsigned max) {
    int first = -1;
    for (unsigned i = 0; i < a->count; i++) if (a->out[i].enabled) { first = (int)i; break; }
    if (first < 0) return 0;
    unsigned n = 0;
    const struct embk_output *f = &d->outputs[first];
    for (unsigned k = 0; k < f->mode_count && k < 16 && n < max; k++) {
        uint32_t mw = f->modes[k].width, mh = f->modes[k].height;
        int all = 1;
        for (unsigned i = 0; i < a->count && all; i++)
            if (a->out[i].enabled && !has_mode(&d->outputs[i], mw, mh)) all = 0;
        for (unsigned j = 0; j < n && all; j++) if (w[j] == mw && h[j] == mh) all = 0;
        if (!all) continue;
        /* Largest first. */
        unsigned at = n;
        while (at > 0 && (uint64_t)w[at - 1] * h[at - 1] < (uint64_t)mw * mh) { w[at] = w[at - 1]; h[at] = h[at - 1]; at--; }
        w[at] = mw; h[at] = mh; n++;
    }
    return n;
}

int arrange_set_mirror(const struct embk_display_layout *d, struct arrange *a, int mirror) {
    if (!!mirror == !!a->mirror) return ARRANGE_OK;
    if (mirror) {
        uint32_t w[16], h[16];
        unsigned n = arrange_common_modes(d, a, w, h, 16);
        if (!n) return ARRANGE_MIRROR_SIZE;
        /* The primary's own size when every display can show it; else the largest they share. */
        int p = primary_of(a);
        uint32_t mw = w[0], mh = h[0];
        for (unsigned k = 0; k < n && p >= 0; k++)
            if (w[k] == a->out[p].width && h[k] == a->out[p].height) { mw = w[k]; mh = h[k]; }
        a->mirror = 1;
        for (unsigned i = 0; i < a->count; i++)
            if (a->out[i].enabled) { a->out[i].width = mw; a->out[i].height = mh; a->out[i].side = ARRANGE_RIGHT; a->out[i].order = 0; a->out[i].offset = 0; }
        return ARRANGE_OK;
    }
    /* Back to extending: each display at its own preferred size, side by side. */
    a->mirror = 0;
    int32_t order = 0;
    for (unsigned i = 0; i < a->count; i++) {
        struct arrange_out *o = &a->out[i];
        if (!o->enabled) continue;
        uint32_t w, h;
        preferred(&d->outputs[i], &w, &h);
        if (w && h) { o->width = w; o->height = h; }
        o->side = ARRANGE_RIGHT; o->order = order++; o->offset = 0;
    }
    return ARRANGE_OK;
}

int arrange_follow(const struct embk_display_layout *was, const struct embk_display_layout *now,
                   int32_t x, int32_t y, int32_t *dx, int32_t *dy) {
    unsigned n = outputs(was);
    int p = -1;
    for (unsigned i = 0; i < n; i++)
        if ((was->outputs[i].flags & EMBK_OUTPUT_ENABLED) && (was->outputs[i].flags & EMBK_OUTPUT_PRIMARY)) p = (int)i;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = p < 0 ? k : k == 0 ? (unsigned)p : k <= (unsigned)p ? k - 1 : k;
        const struct embk_output *o = &was->outputs[i];
        if (!(o->flags & EMBK_OUTPUT_ENABLED) || x < o->x || y < o->y ||
            x >= o->x + (int64_t)o->width || y >= o->y + (int64_t)o->height) continue;
        if (i >= outputs(now) || !(now->outputs[i].flags & EMBK_OUTPUT_ENABLED)) return 0;
        *dx = now->outputs[i].x - o->x;
        *dy = now->outputs[i].y - o->y;
        return 1;
    }
    return 0;
}

const char *arrange_error(int code) {
    switch (code) {
    case ARRANGE_OK:           return "";
    case ARRANGE_NONE:         return "At least one display must stay in use.";
    case ARRANGE_PRIMARY:      return "The primary display must be one that is in use.";
    case ARRANGE_OVERLAP:      return "Two displays would overlap. Put one of them on another side.";
    case ARRANGE_DETACHED:     return "Every display must share an edge with another one.";
    case ARRANGE_TOO_LARGE:    return "Together these displays are larger than the graphics can show (8192 × 4320).";
    case ARRANGE_MIRROR_SIZE:  return "Mirrored displays need a resolution all of them can show.";
    case ARRANGE_DISCONNECTED: return "That display is no longer connected.";
    case ARRANGE_MODE:         return "A display cannot show the chosen resolution.";
    case ARRANGE_MISMATCH:     return "The displays changed. Look again before applying.";
    case ARRANGE_TOO_MANY:     return "This computer cannot show that many displays at once. Turn one off first.";
    default:                   return "This arrangement cannot be shown.";
    }
}

/* ---- saving ---------------------------------------------------------------- */

static uint32_t fnv(uint32_t h, const char *s, unsigned cap) {
    for (unsigned k = 0; k < cap && s[k]; k++) h = (h ^ (uint8_t)s[k]) * 16777619u;
    return (h ^ 0xffu) * 16777619u;                   /* a separator: "ab"+"c" is not "a"+"bc" */
}

uint32_t arrange_output_id(const struct embk_output *o) {
    uint32_t h = 2166136261u;
    h = fnv(h, o->name, sizeof o->name);
    h = fnv(h, o->manufacturer, sizeof o->manufacturer);
    h = fnv(h, o->model, sizeof o->model);
    uint32_t mm[2] = { o->width_mm, o->height_mm };
    for (unsigned k = 0; k < sizeof mm; k++) h = (h ^ ((const uint8_t *)mm)[k]) * 16777619u;
    return h ? h : 1u;
}

unsigned arrange_connected(const struct embk_display_layout *d) {
    unsigned n = 0;
    for (unsigned i = 0; i < outputs(d); i++) n += (d->outputs[i].flags & EMBK_OUTPUT_CONNECTED) ? 1u : 0u;
    return n;
}

uint32_t arrange_connected_set(const struct embk_display_layout *d) {
    uint32_t sum = 0, n = 0;
    for (unsigned i = 0; i < outputs(d); i++) {
        if (!(d->outputs[i].flags & EMBK_OUTPUT_CONNECTED)) continue;
        uint32_t v = arrange_output_id(&d->outputs[i]) * 0x9e3779b1u;
        sum += v ^ (v >> 15);                          /* order-free: the same monitors, whichever port enumerates first */
        n++;
    }
    if (!n) return 0;
    sum = (sum ^ n) * 0x85ebca6bu;
    return sum ? sum : 1u;
}

void arrange_save(const struct embk_display_layout *d, struct display_saved *s) {
    memset(s, 0, sizeof *s);
    s->mirror = (d->flags & EMBK_LAYOUT_MIRROR) != 0;
    for (unsigned i = 0; i < outputs(d) && s->count < (int)EMBK_DISPLAY_MAX_OUTPUTS; i++) {
        const struct embk_output *o = &d->outputs[i];
        if (!(o->flags & EMBK_OUTPUT_CONNECTED)) continue;
        int on = (o->flags & EMBK_OUTPUT_ENABLED) && o->width && o->height;
        s->out[s->count].id = arrange_output_id(o);
        s->out[s->count].enabled = on;
        s->out[s->count].primary = on && (o->flags & EMBK_OUTPUT_PRIMARY);
        s->out[s->count].width = on ? (int)o->width : 0;
        s->out[s->count].height = on ? (int)o->height : 0;
        s->out[s->count].x = on ? o->x : 0;
        s->out[s->count].y = on ? o->y : 0;
        s->count++;
    }
}

int arrange_saved_request(const struct embk_display_layout *d, const struct display_saved *s,
                          struct embk_layout_request *r) {
    memset(r, 0, sizeof *r);
    r->version = EMBK_DISPLAY_LAYOUT_VERSION;
    r->generation = d->generation;
    r->flags = s->mirror ? EMBK_LAYOUT_MIRROR : 0;
    r->count = outputs(d);
    if (s->count <= 0 || s->count > (int)EMBK_DISPLAY_MAX_OUTPUTS || (unsigned)s->count != arrange_connected(d))
        return ARRANGE_MISMATCH;
    int used[EMBK_DISPLAY_MAX_OUTPUTS] = { 0 };
    for (unsigned i = 0; i < r->count; i++) {
        const struct embk_output *o = &d->outputs[i];
        if (!(o->flags & EMBK_OUTPUT_CONNECTED)) continue;
        uint32_t id = arrange_output_id(o);
        int found = -1;
        for (int k = 0; k < s->count; k++) if (!used[k] && s->out[k].id == id) { found = k; break; }
        if (found < 0) return ARRANGE_MISMATCH;
        used[found] = 1;
        if (!s->out[found].enabled) continue;
        if (s->out[found].width <= 0 || s->out[found].height <= 0) return ARRANGE_MODE;
        r->out[i].enabled = 1;
        r->out[i].primary = s->out[found].primary != 0;
        r->out[i].width = (uint32_t)s->out[found].width; r->out[i].height = (uint32_t)s->out[found].height;
        r->out[i].x = s->out[found].x; r->out[i].y = s->out[found].y;
    }
    int rc = check(d, r);
    if (rc) return rc;
    return arrange_differs(d, r) ? 1 : 0;
}
