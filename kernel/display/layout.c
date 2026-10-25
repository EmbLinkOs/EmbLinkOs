/* layout.c -- several outputs, one desktop. See layout.h. */
#include "display/layout.h"

static int64_t right(const struct layout_out *o) { return (int64_t)o->x + o->w; }
static int64_t bottom(const struct layout_out *o) { return (int64_t)o->y + o->h; }

static int overlap(const struct layout_out *a, const struct layout_out *b)
{
    return a->x < right(b) && b->x < right(a) && a->y < bottom(b) && b->y < bottom(a);
}

/* Sharing a stretch of edge, not just a corner. */
static int touch(const struct layout_out *a, const struct layout_out *b)
{
    int vert = (right(a) == b->x || right(b) == a->x) && a->y < bottom(b) && b->y < bottom(a);
    int horiz = (bottom(a) == b->y || bottom(b) == a->y) && a->x < right(b) && b->x < right(a);
    return vert || horiz;
}

static void bbox(const struct layout *l, int64_t *x0, int64_t *y0, int64_t *x1, int64_t *y1)
{
    *x0 = *y0 = INT64_MAX;
    *x1 = *y1 = INT64_MIN;
    for (unsigned i = 0; i < l->count; i++) {
        const struct layout_out *o = &l->out[i];
        if (!o->enabled) continue;
        if (o->x < *x0) *x0 = o->x;
        if (o->y < *y0) *y0 = o->y;
        if (right(o) > *x1) *x1 = right(o);
        if (bottom(o) > *y1) *y1 = bottom(o);
    }
}

int layout_validate(const struct layout *l, uint32_t *dw, uint32_t *dh)
{
    unsigned enabled = 0, primaries = 0, first = 0;
    for (unsigned i = 0; i < l->count; i++) {
        const struct layout_out *o = &l->out[i];
        if (!o->enabled) continue;
        if (!o->connected) return LAYOUT_DISCONNECTED;
        if (!o->w || !o->h) return LAYOUT_TOO_LARGE;
        if (!enabled) first = i;
        enabled++;
        primaries += o->primary ? 1u : 0u;
    }
    if (!enabled) return LAYOUT_NONE;
    if (primaries != 1) return LAYOUT_PRIMARY;
    if (l->mirror) {
        for (unsigned i = 0; i < l->count; i++)
            if (l->out[i].enabled && (l->out[i].w != l->out[first].w || l->out[i].h != l->out[first].h))
                return LAYOUT_MIRROR_SIZE;
        if (l->out[first].w > LAYOUT_MAX_W || l->out[first].h > LAYOUT_MAX_H ||
            (uint64_t)l->out[first].w * l->out[first].h > LAYOUT_MAX_PIXELS)
            return LAYOUT_TOO_LARGE;
        if (dw) *dw = l->out[first].w;
        if (dh) *dh = l->out[first].h;
        return LAYOUT_OK;
    }
    for (unsigned i = 0; i < l->count; i++)
        for (unsigned j = i + 1; j < l->count; j++)
            if (l->out[i].enabled && l->out[j].enabled && overlap(&l->out[i], &l->out[j])) return LAYOUT_OVERLAP;
    /* Every output reachable from the first by shared edges. */
    unsigned reached = 1u << first, grew = 1;
    while (grew) {
        grew = 0;
        for (unsigned i = 0; i < l->count; i++) {
            if (!l->out[i].enabled || (reached & (1u << i))) continue;
            for (unsigned j = 0; j < l->count; j++)
                if ((reached & (1u << j)) && touch(&l->out[i], &l->out[j])) { reached |= 1u << i; grew = 1; break; }
        }
    }
    for (unsigned i = 0; i < l->count; i++)
        if (l->out[i].enabled && !(reached & (1u << i))) return LAYOUT_DETACHED;
    int64_t x0, y0, x1, y1;
    bbox(l, &x0, &y0, &x1, &y1);
    uint64_t w = (uint64_t)(x1 - x0), h = (uint64_t)(y1 - y0);
    if (w > LAYOUT_MAX_W || h > LAYOUT_MAX_H || w * h > LAYOUT_MAX_PIXELS) return LAYOUT_TOO_LARGE;
    if (dw) *dw = (uint32_t)w;
    if (dh) *dh = (uint32_t)h;
    return LAYOUT_OK;
}

void layout_normalize(struct layout *l)
{
    if (l->mirror) {
        for (unsigned i = 0; i < l->count; i++) l->out[i].x = l->out[i].y = 0;
        return;
    }
    int64_t x0, y0, x1, y1;
    bbox(l, &x0, &y0, &x1, &y1);
    if (x0 == INT64_MAX) return;
    for (unsigned i = 0; i < l->count; i++) {
        if (!l->out[i].enabled) continue;
        l->out[i].x = (int32_t)(l->out[i].x - x0);
        l->out[i].y = (int32_t)(l->out[i].y - y0);
    }
}

void layout_place_right(struct layout *l, unsigned i)
{
    int64_t edge = 0;
    int32_t top = 0;
    int any = 0;
    for (unsigned k = 0; k < l->count; k++) {
        const struct layout_out *o = &l->out[k];
        if (k == i || !o->enabled) continue;
        if (!any || right(o) > edge) edge = right(o);
        if (o->primary) top = o->y;
        any = 1;
    }
    l->out[i].x = any ? (int32_t)edge : 0;
    l->out[i].y = top;
}

unsigned layout_rects(const struct layout *l, struct layout_rect *r, unsigned max)
{
    unsigned n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < l->count && n < max; i++) {
            const struct layout_out *o = &l->out[i];
            if (!o->enabled || (pass == 0) != (o->primary != 0)) continue;
            r[n++] = (struct layout_rect){ o->x, o->y, o->w, o->h };
        }
    return n;
}

int layout_rect_at(const struct layout_rect *r, unsigned n, int32_t x, int32_t y)
{
    for (unsigned i = 0; i < n; i++)
        if (x >= r[i].x && y >= r[i].y && x < r[i].x + (int64_t)r[i].w && y < r[i].y + (int64_t)r[i].h)
            return (int)i;
    return -1;
}

void layout_clamp_point(const struct layout_rect *r, unsigned n, int32_t *x, int32_t *y)
{
    if (!n || layout_rect_at(r, n, *x, *y) >= 0) return;
    int64_t best = INT64_MAX;
    int32_t bx = *x, by = *y;
    for (unsigned i = 0; i < n; i++) {
        int64_t cx = *x < r[i].x ? r[i].x : *x >= r[i].x + (int64_t)r[i].w ? r[i].x + (int64_t)r[i].w - 1 : *x;
        int64_t cy = *y < r[i].y ? r[i].y : *y >= r[i].y + (int64_t)r[i].h ? r[i].y + (int64_t)r[i].h - 1 : *y;
        int64_t d = (cx - *x) * (cx - *x) + (cy - *y) * (cy - *y);
        if (d < best) { best = d; bx = (int32_t)cx; by = (int32_t)cy; }
    }
    *x = bx;
    *y = by;
}

int layout_rehome(const struct layout_rect *r, unsigned n, int32_t *x, int32_t *y, uint32_t w, uint32_t h)
{
    if (!n) return 0;
    int32_t cx = *x + (int32_t)(w / 2), cy = *y + (int32_t)(h / 2);
    if (layout_rect_at(r, n, cx, cy) >= 0) return 0;
    const struct layout_rect *p = &r[0];
    int64_t nx = *x, ny = *y;
    /* Keep the offset it had, modulo the primary's size, then fit it in. */
    if (nx < p->x || nx >= p->x + (int64_t)p->w) nx = p->x + ((nx % (int64_t)p->w) + p->w) % p->w;
    if (ny < p->y || ny >= p->y + (int64_t)p->h) ny = p->y + ((ny % (int64_t)p->h) + p->h) % p->h;
    if (nx + w > p->x + (int64_t)p->w) nx = p->x + (int64_t)p->w - w;
    if (ny + h > p->y + (int64_t)p->h) ny = p->y + (int64_t)p->h - h;
    if (nx < p->x) nx = p->x;
    if (ny < p->y) ny = p->y;
    *x = (int32_t)nx;
    *y = (int32_t)ny;
    return 1;
}
