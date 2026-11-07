/* ui/backend/scene_render.c -- EmbLink UI Piece 4a: the traversal driver
 * (see scene_render.h). Implements Section 5 (grouped opacity) and Section 6
 * (the dirty-rect algorithm, specified as explicit steps precisely because
 * it's easy to get subtly wrong). */

#include "scene_render.h"
#if defined(SCENE_TRACE) || defined(SCROLL_DEBUG) || defined(PROMOTE_TRACE)
#include <stdio.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* the reduced effects tier (scene_render_set_reduced): glass without its blur */
static int g_effects_reduced;


/* ------------------------------------------------------------------------- */
/* geometry helpers                                                          */
/* ------------------------------------------------------------------------- */

struct frect { float x, y, w, h; };   /* w<=0 || h<=0 == empty */

static struct frect frect_empty(void) { struct frect f = {0,0,0,0}; return f; }
static int frect_is_empty(struct frect f) { return f.w <= 0 || f.h <= 0; }

static struct frect frect_union(struct frect a, struct frect b) {
    if (frect_is_empty(a)) return b;
    if (frect_is_empty(b)) return a;
    float x0 = a.x < b.x ? a.x : b.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    struct frect f = { x0, y0, x1 - x0, y1 - y0 };
    return f;
}
static int frect_overlap(struct frect a, struct frect b) {
    if (frect_is_empty(a) || frect_is_empty(b)) return 0;
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

/* World-space axis-aligned bounding box of a node's local (0,0,w,h) rect. For
 * pure translate+scale this is exact; for rotation it's the rotated rect's
 * AABB (fine for the CPU backend, which draws axis-aligned rects). */
static struct frect node_world_aabb(const struct scene_node *n, const float world[16]) {
    float cx[4] = { 0, n->width, 0, n->width };
    float cy[4] = { 0, 0, n->height, n->height };
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < 4; i++) {
        float X = world[0] * cx[i] + world[4] * cy[i] + world[12];
        float Y = world[1] * cx[i] + world[5] * cy[i] + world[13];
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    struct frect f = { minx, miny, maxx - minx, maxy - miny };
    return f;
}

/* Union of a whole subtree's world AABBs (Section 5's group bounding rect). */
static int g_sb_depth;
static struct frect subtree_bounds(struct scene_arena *a, struct node_handle h,
                                   const float wparent[16]) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || g_sb_depth > 1024) return frect_empty();
    g_sb_depth++;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct frect b = node_world_aabb(n, world);
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        b = frect_union(b, subtree_bounds(a, c, world));
        c = next;
    }
    g_sb_depth--;
    return b;
}

/* ------------------------------------------------------------------------- */
/* dirty-rect accumulation (Section 6)                                       */
/* ------------------------------------------------------------------------- */

static void dirty_add(struct scene_renderer *r, struct frect f) {
    if (r->full || frect_is_empty(f)) return;
    if (r->n_dirty >= 16) {
        /* AT THE CAP, MERGE -- into the rect that grows least taking it in.
         * This used to repaint everything instead, and a group moving is
         * every one of its children moving, each adding its old place and its
         * new: a menu of nine things fading in repainted its whole window
         * every frame. Their places all lie inside the group's own, so the
         * merged rects stay about the size of what really changed. */
        float x0 = floorf(f.x), y0 = floorf(f.y), x1 = ceilf(f.x + f.w), y1 = ceilf(f.y + f.h);
        int best = 0; float grow = 0;
        for (int i = 0; i < r->n_dirty; i++) {
            struct clip_rect *d = &r->dirty[i];
            float ux0 = d->x < x0 ? d->x : x0, uy0 = d->y < y0 ? d->y : y0;
            float ux1 = d->x + d->w > x1 ? d->x + d->w : x1, uy1 = d->y + d->h > y1 ? d->y + d->h : y1;
            float g = (ux1 - ux0) * (uy1 - uy0) - d->w * d->h;
            if (i == 0 || g < grow) { grow = g; best = i; }
        }
        struct clip_rect *d = &r->dirty[best];
        float ux0 = d->x < x0 ? d->x : x0, uy0 = d->y < y0 ? d->y : y0;
        float ux1 = d->x + d->w > x1 ? d->x + d->w : x1, uy1 = d->y + d->h > y1 ? d->y + d->h : y1;
        d->x = ux0; d->y = uy0; d->w = ux1 - ux0; d->h = uy1 - uy0;
        return;
    }
    /* WHOLE PIXELS, rounded outward. A pixel is repainted when its CENTRE is in
     * a dirty rect, but a node at a fractional place stains the pixel its edge
     * only partly covers: an anti-aliased edge does, and text does more, its
     * pen truncated to the pixel at or left of its x. A title at x 480.6 inked
     * pixel 480, its old footprint's rect began at 480.6 and missed 480.5, and
     * a sliver of the old title stayed beside the new one. */
    float x0 = floorf(f.x), y0 = floorf(f.y);
    float x1 = ceilf(f.x + f.w), y1 = ceilf(f.y + f.h);
    struct clip_rect c = { x0, y0, x1 - x0, y1 - y0, 0.0f };
    r->dirty[r->n_dirty++] = c;
}
static int dirty_hits(struct scene_renderer *r, struct frect f) {
    if (r->full || r->no_cull) return 1;
    for (int i = 0; i < r->n_dirty; i++) {
        struct frect d = { r->dirty[i].x, r->dirty[i].y, r->dirty[i].w, r->dirty[i].h };
        if (frect_overlap(f, d)) return 1;
    }
    return 0;
}

/* one gathered node (document order == paint order) */
struct noderec {
    struct node_handle h;
    struct frect       aabb;
    struct frect       foot;    /* aabb grown by the shadow's reach -- what the
                                 * node actually STAINS on the target, and
                                 * therefore what its ghost occupies when it
                                 * moves or dies */
    int                is_blur;
    int                has_group;
    struct frect       group_bounds;
    int                dirty;
    /* scroll-detection scratch (Step 1): the cached old footprint, and the
     * classification of WHY this node is dirty -- a pure move can be blitted,
     * a content change cannot. */
    struct frect       old;
    int                has_old;
    int                own_dirty;
    int                moved;
    /* a pure translation (moved, same size) and by how much -- and whether it
     * went some OTHER way than the frame's scroll did (Step 1a) */
    int                trans;
    float              tdx, tdy;
    int                odd;
    int                clips;
    /* What this node can actually STAIN: `foot` intersected with every
     * clipping ancestor. A node scrolled past the bottom of its container has
     * a footprint below the clip and NOTHING visible there -- the scroll
     * classifier has to reason about the latter, or a document taller than its
     * viewport permanently vetoes its own fast path. Empty when fully clipped,
     * and frect_overlap treats empty as touching nothing. */
    struct frect       vis;
    struct frect       clipbox;      /* the ancestor clip itself, for the OLD foot */
    /* a node that paints nothing whose compositing changed (opacity, scale):
     * every pixel of its subtree is new, though it stains none itself */
    struct frect       extent;
    int                has_extent;
    int                has_clipbox;
};

/* a ∩ b, empty if they miss */
static struct frect frect_isect(struct frect a, struct frect b) {
    float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return frect_empty();
    struct frect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* the painted footprint: the AABB grown by shadow reach (mirrors paint_visual) */
static struct frect node_footprint(const struct scene_node *n, struct frect ab) {
    if (!n->shadow_enabled) return ab;
    float m  = n->shadow_blur_radius + 2.0f;
    float x0 = ab.x, y0 = ab.y, x1 = ab.x + ab.w, y1 = ab.y + ab.h;
    float sx0 = ab.x + n->shadow_dx - m,        sy0 = ab.y + n->shadow_dy - m;
    float sx1 = ab.x + ab.w + n->shadow_dx + m, sy1 = ab.y + ab.h + n->shadow_dy + m;
    if (sx0 < x0) x0 = sx0;
    if (sy0 < y0) y0 = sy0;
    if (sx1 > x1) x1 = sx1;
    if (sy1 > y1) y1 = sy1;
    struct frect f = { x0, y0, x1 - x0, y1 - y0 };
    return f;
}

static struct frect subtree_footprint(struct scene_arena *a, struct node_handle h, const float wparent[16]);

static int node_paints_nothing(const struct scene_node *n) {
    if (n->clip_children || n->shadow_enabled || n->backdrop_blur_enabled) return 0;
    if (n->kind == SCENE_NODE_GROUP) return 1;
    if (n->kind != SCENE_NODE_RECT || n->border_width > 0) return 0;
    const struct paint *p = &n->data.rect.fill;
    return p->kind == PAINT_NONE || (p->kind == PAINT_SOLID && p->solid.a <= 0.002f);
}

/* A layer host keeps promoted subtrees this frame: the window leaves them out
 * (scene_render_set_layer_host). */
static int g_promote_live;
/* A PROMOTED NODE INSIDE ANOTHER is a layer of its own, made after (so above)
 * the one round it: not the outer one's pixels, footprint, signature or
 * damage. A sheet's dimming and the sheet are two layers, each on its own
 * motion. */
static int nested_layer(const struct scene_node *n) { return n && n->promoted && g_promote_live; }

static void gather(struct scene_arena *a, struct node_handle h, const float wparent[16],
                   int has_group, struct frect gbounds,
                   int has_clip, struct frect clipr,
                   struct noderec *list, int *n, int cap) {
    struct scene_node *node = scene_resolve(a, h);
    if (!node || *n >= cap) return;
    if (node->promoted && g_promote_live) return;   /* not the window's: a layer's */
    float local[16], world[16];
    scene_trs_to_matrix(node, local);
    scene_mat4_mul(wparent, local, world);

    struct noderec *rec = &list[(*n)++];
    rec->h = h;
    rec->aabb = node_world_aabb(node, world);
    rec->foot = node_footprint(node, rec->aabb);
    /* A NODE THAT PAINTS NOTHING STAINS NOTHING: a group, or a box with no
     * fill, border, shadow or glass. Its coming, going and moving change no
     * pixel -- its children, which do, mark their own -- and it used to mark
     * its whole area: a menu's window-sized transparent scrim opening was a
     * repaint of the whole window. Not a box that clips, though: changing the
     * clip changes what its children show. */
    rec->has_extent = 0;
    if (node_paints_nothing(node)) {
        rec->foot = frect_empty();
        if (node->dirty_comp) { rec->extent = subtree_footprint(a, h, wparent); rec->has_extent = 1; }
    }
    rec->is_blur = node->backdrop_blur_enabled;
    rec->has_group = has_group;
    rec->group_bounds = gbounds;
    rec->dirty = 0;
    rec->has_old = 0; rec->own_dirty = 0; rec->moved = 0;
    rec->trans = 0; rec->odd = 0; rec->tdx = rec->tdy = 0;
    rec->clips = node->clip_children ? 1 : 0;
    rec->vis = has_clip ? frect_isect(rec->foot, clipr) : rec->foot;
    rec->clipbox = clipr; rec->has_clipbox = has_clip;

    int child_has_clip = has_clip;
    struct frect child_clip = clipr;
    if (node->clip_children) {
        child_clip = has_clip ? frect_isect(clipr, rec->aabb) : rec->aabb;
        child_has_clip = 1;
    }
    int child_group = has_group;
    struct frect child_gb = gbounds;
    if (node->opacity < 1.0f || node->group_scale < 1.0f) {   /* this node starts (or nests) a group */
        child_group = 1;
        child_gb = subtree_bounds(a, h, wparent);
    }
    struct node_handle c = node->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        gather(a, c, world, child_group, child_gb, child_has_clip, child_clip, list, n, cap);
        c = next;
    }
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paint_visual(struct scene_renderer *r, struct scene_node *n,
                         const float world[16], struct render_target *target,
                         float ox, float oy) {
    struct render_backend *be = r->be;
    struct frect ab = node_world_aabb(n, world);

    /* Node-level cull: if this node's painted footprint doesn't touch the dirty
     * region, its pixels are already correct on-target -- skip ALL per-pixel work
     * (text/rect/shadow). This is what makes an incremental frame cost scale with
     * the changed area, not the node count. dirty_hits() returns true under a
     * full repaint, so nothing is wrongly skipped then. The footprint is grown by
     * the shadow's reach so a shadow straddling the dirty edge still repaints. */
    float px0 = ab.x, py0 = ab.y, px1 = ab.x + ab.w, py1 = ab.y + ab.h;
    if (n->shadow_enabled) {
        float m = n->shadow_blur_radius + 2.0f;
        float sx0 = ab.x + n->shadow_dx - m,          sy0 = ab.y + n->shadow_dy - m;
        float sx1 = ab.x + ab.w + n->shadow_dx + m,   sy1 = ab.y + ab.h + n->shadow_dy + m;
        if (sx0 < px0) px0 = sx0;
        if (sy0 < py0) py0 = sy0;
        if (sx1 > px1) px1 = sx1;
        if (sy1 > py1) py1 = sy1;
    }
    /* in the WINDOW's coordinates, as the dirty rects are -- not the target's:
     * inside a group the target is its own buffer, offset by (ox, oy) */
    struct frect foot = { px0, py0, px1 - px0, py1 - py0 };
#ifdef SCENE_TRACE
    fprintf(stderr, "paint kind=%d layer=%d rect=(%.0f,%.0f %.0fx%.0f) hit=%d\n",
            n->kind, n->layer, ab.x, ab.y, ab.w, ab.h, dirty_hits(r, foot));
#endif
    if (!dirty_hits(r, foot)) return;

    float dx = ab.x - ox, dy = ab.y - oy, w = ab.w, h = ab.h;

    switch (n->kind) {
        case SCENE_NODE_RECT:
            if (n->shadow_enabled)
                be->draw_shadow(target, dx, dy, w, h, n->corner_radius,
                                n->shadow_dx, n->shadow_dy, n->shadow_blur_radius, n->shadow_color);
            {
                struct paint fill = n->data.rect.fill;
                if (n->backdrop_blur_enabled && n == r->glass_root) {
                    /* a layer's glass: the compositor frosts what is behind
                     * it; here only the tint, as it is */
                } else if (n->backdrop_blur_enabled && g_effects_reduced) {
                    /* the reduced tier: no blur to compute -- the glass's own tint, whole */
                    if (fill.kind == PAINT_SOLID) fill.solid.a = 1.0f;
                } else if (n->backdrop_blur_enabled) {
                    if (r->backdrop && r->backdrop != target && be->draw_backdrop_blur_from)
                        be->draw_backdrop_blur_from(target, dx, dy, w, h, n->corner_radius, n->backdrop_blur_radius,
                                                    r->backdrop, ab.x - r->bd_ox, ab.y - r->bd_oy);
                    else
                        be->draw_backdrop_blur(target, dx, dy, w, h, n->corner_radius, n->backdrop_blur_radius);
                }
                be->draw_rect(target, dx, dy, w, h, n->corner_radius, &fill, 1.0f);
            }
            if (n->border_width > 0 && be->draw_border)
                be->draw_border(target, dx, dy, w, h, n->corner_radius, n->border_width,
                                n->border_color, &n->border_paint);
            break;
        case SCENE_NODE_IMAGE:
            be->draw_image(target, dx, dy, w, h, n->data.image.pixels,
                           n->data.image.w, n->data.image.h, n->data.image.w * 4,
                           n->data.image.fmt, 1.0f,
                           n->data.image.tinted ? &n->data.image.tint : NULL);
            break;
        case SCENE_NODE_TEXT:
            /* the run's own background (selection), behind its glyphs */
            if (n->data.text.bg.a > 0.001f) {
                struct paint bp; bp.kind = PAINT_SOLID; bp.solid = n->data.text.bg;
                be->draw_rect(target, dx, dy, w, h, 0.0f, &bp, 1.0f);
            }
            if (be->draw_text)
                be->draw_text(target, dx, dy, n->data.text.utf8, n->data.text.font_handle,
                              n->data.text.size_px, n->data.text.color, 1.0f,
                              &n->data.text.paint, w, h, !n->data.text.no_wrap);
            break;
        case SCENE_NODE_GROUP:
        default: break;
    }
}

/* ---- the group cache (scene_render.h struct group_cache) ----------------- */

/* Did anything inside the group change this frame, other than how the group
 * itself is composited? Any descendant touched at all (moved within it,
 * repainted, resized), or the root's own look. `sig` gathers the subtree's
 * shape -- which nodes, in what order -- because a child added or taken away
 * marks nothing dirty; `blur`, whether any of it is glass, whose pixels
 * depend on what lies behind and so cannot be kept. */
static int g_gc_depth;
static int group_changed_walk(struct scene_arena *a, struct node_handle h, int is_root,
                              uint32_t *sig, int *blur) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || g_gc_depth > 1024) return 1;
    *sig = (*sig ^ (h.index * 2654435761u) ^ (h.generation << 7) ^ (uint32_t)n->kind) * 16777619u;
    if (n->backdrop_blur_enabled) *blur = 1;
    int changed = is_root ? n->dirty_look : n->dirty;
    g_gc_depth++;
    struct node_handle c = n->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        if (!nested_layer(cn)) changed |= group_changed_walk(a, c, 0, sig, blur);
        c = next;
    }
    g_gc_depth--;
    return changed;
}
static int group_changed(struct scene_arena *a, struct node_handle h, uint32_t *sig, int *blur) {
    *sig = 2166136261u; *blur = 0;
    return group_changed_walk(a, h, 1, sig, blur);
}

#define GROUP_CACHE_BYTES (24u << 20)   /* all kept groups together */

static struct group_cache *group_find(struct scene_renderer *r, struct node_handle h) {
    for (int i = 0; i < SCENE_GROUP_CACHE; i++)
        if (r->groups[i].index == h.index && r->groups[i].generation == h.generation && h.index)
            return &r->groups[i];
    return 0;
}
static void group_drop(struct group_cache *g) {
    free(g->px);
    memset(g, 0, sizeof *g);
}
static uint64_t group_bytes(const struct scene_renderer *r) {
    uint64_t n = 0;
    for (int i = 0; i < SCENE_GROUP_CACHE; i++) n += (uint64_t)r->groups[i].cap * 4;
    return n;
}
/* a slot for a group about to be painted: a free one, or one not met yet this
 * frame; none when keeping it would pass the byte cap (it is painted each time) */
static struct group_cache *group_take(struct scene_renderer *r, struct node_handle h, uint32_t px) {
    struct group_cache *g = 0;
    for (int i = 0; i < SCENE_GROUP_CACHE && !g; i++) if (!r->groups[i].index) g = &r->groups[i];
    for (int i = 0; i < SCENE_GROUP_CACHE && !g; i++) if (!r->groups[i].used) { group_drop(&r->groups[i]); g = &r->groups[i]; }
    if (!g) return 0;
    if (group_bytes(r) + (uint64_t)px * 4 > GROUP_CACHE_BYTES) return 0;
    g->index = h.index; g->generation = h.generation; g->used = 1;
    return g;
}
static int group_buffer(struct group_cache *g, uint32_t w, uint32_t h) {
    uint32_t need = w * h;
    if (g->cap < need) {
        uint32_t *p = (uint32_t *)realloc(g->px, (size_t)need * 4);
        if (!p) return 0;
        g->px = p; g->cap = need;
    }
    g->w = w; g->h = h;
    return 1;
}

/* Elevated subtrees found during the flow pass, painted afterwards in
 * (layer, encounter) order. wparent is stashed so the deferred pass rebuilds
 * the identical world transform; the clip stack is naturally empty by then,
 * which is exactly popover semantics (a dropdown escapes its strip's clip). */
#define ELEV_MAX 32
struct elev_list {
    struct { struct node_handle h; float wparent[16]; int layer; } e[ELEV_MAX];
    int n;
};

static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy,
                        int in_elev, struct elev_list *el);
static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy,
                              int in_elev, struct elev_list *el);

/* Depth-bounded wrapper: a corrupt scene tree with a parent<->child cycle would
 * otherwise recurse forever here (no fault, just a hang). Cap the depth well
 * above any real UI nesting so a cycle degrades instead of hanging. */
static int g_render_depth;
static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy,
                        int in_elev, struct elev_list *el) {
    if (g_render_depth > 1024) return;
    g_render_depth++;
    render_node_inner(r, a, h, wparent, target, ox, oy, in_elev, el);
    g_render_depth--;
}

/* Paint a node's own visual (at opacity 1) + recurse children -- used for the
 * ROOT of a group's offscreen pass, where the group's own opacity is applied
 * once at the final blit, not here (Section 5 step 2). */
static void render_subtree_opaque(struct scene_renderer *r, struct scene_arena *a,
                                  struct node_handle h, const float wparent[16],
                                  struct render_target *target, float ox, float oy) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy, 1, 0);   /* groups flatten: inline */
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy,
                              int in_elev, struct elev_list *el) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (n->promoted && r->host) return;     /* a layer's, painted in the promoted pass */

    /* an elevated subtree met during the FLOW pass: stash it for the deferred
     * pass instead of painting it here in document position */
    if (!in_elev && el && n->layer > 0 && el->n < ELEV_MAX) {
        el->e[el->n].h = h;
        for (int i = 0; i < 16; i++) el->e[el->n].wparent[i] = wparent[i];
        el->e[el->n].layer = n->layer;
        el->n++;
        return;
    }

    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    if (n->opacity < 1.0f || n->group_scale < 1.0f) {
        /* Section 5: grouped offscreen compositing -- render the whole subtree
         * at full opacity into a scratch target, then blit once with the
         * group's opacity, so overlapping children blend against each other at
         * full strength (no seam) and only the flattened result is dimmed. */
        struct frect b = subtree_bounds(a, h, wparent);
        int bx = (int)floorf(b.x), by = (int)floorf(b.y);
        int bw = (int)ceilf(b.x + b.w) - bx, bh = (int)ceilf(b.y + b.h) - by;
        if (bw <= 0 || bh <= 0) return;

        /* KEPT PIXELS: the group as it was last painted, if nothing in it has
         * changed since but how it is composited (struct group_cache). */
        uint32_t sig = 0; int blur = 0;
        int changed = group_changed(a, h, &sig, &blur);
        float fx = b.x - (float)bx, fy = b.y - (float)by;
        struct group_cache *gc = group_find(r, h);
        if (gc) gc->used = 1;
        if (!dirty_hits(r, b)) {          /* whole group outside dirty -> skip */
            if (gc && changed) group_drop(gc);   /* what it kept is no longer it */
            return;
        }
        int reuse = gc && !changed && !blur && gc->px && gc->w == (uint32_t)bw && gc->h == (uint32_t)bh &&
                    gc->sig == sig && fabsf(gc->fx - fx) < 1e-3f && fabsf(gc->fy - fy) < 1e-3f &&
                    gc->reduced == g_effects_reduced;
        struct render_target kept;
        struct render_target *scratch = 0;
        if (reuse) {
            r->group_hits++;
            memset(&kept, 0, sizeof kept);
            kept.pixels = gc->px; kept.width = gc->w; kept.height = gc->h; kept.stride = gc->w * 4;
            kept.format = EMBK_PIXFMT_BGRA8888_PRE;
            scratch = &kept;
        } else {
            r->group_paints++;
            if (!blur && !gc) gc = group_take(r, h, (uint32_t)bw * (uint32_t)bh);
            if (!blur && gc && group_buffer(gc, (uint32_t)bw, (uint32_t)bh)) {
                memset(&kept, 0, sizeof kept);
                kept.pixels = gc->px; kept.width = gc->w; kept.height = gc->h; kept.stride = gc->w * 4;
                kept.format = EMBK_PIXFMT_BGRA8888_PRE;
                memset(gc->px, 0, (size_t)bw * (size_t)bh * 4);   /* transparent, so it composites */
                scratch = &kept;
                gc->fx = fx; gc->fy = fy; gc->sig = sig; gc->reduced = g_effects_reduced;
            } else {
                if (gc) group_drop(gc);                   /* glass inside: painted each time */
                scratch = cpu_scratch_acquire((uint32_t)bw, (uint32_t)bh);
            }
            if (scratch) {
                struct clip_saved sv;
                /* glass in the group blurs what the group lands on: the outermost
                 * group's target (a nested group's is a half-rendered scratch) */
                int outer = !r->backdrop;
                if (outer) { r->backdrop = target; r->bd_ox = ox; r->bd_oy = oy; }
                int was = r->no_cull; r->no_cull = 1;
                cpu_clip_save_and_clear(&sv);   /* isolate scratch coords + lift dirty */
                render_subtree_opaque(r, a, h, wparent, scratch, (float)bx, (float)by);
                cpu_clip_restore(&sv);
                r->no_cull = was;
                if (outer) r->backdrop = 0;
            }
        }
        if (scratch) {
            /* blit flattened result; coverage_at clips this to the dirty union */
            /* flattened subtree: already-rendered pixels, never a stencil */
            /* ...smaller toward its anchor when the group is scaled (inside its
             * own bounds, so what the dirty rects cover still covers it) */
            float s = n->group_scale < 1.0f ? n->group_scale : 1.0f;
            float dw = (float)bw * s, dh = (float)bh * s;
            float dx = (float)bx + ((float)bw - dw) * n->anchor_x, dy = (float)by + ((float)bh - dh) * n->anchor_y;
            r->be->draw_image(target, dx - ox, dy - oy, dw, dh,
                              scratch->pixels, (uint32_t)bw, (uint32_t)bh, scratch->stride,
                              scratch->format, n->opacity, NULL);
            if (scratch != &kept) cpu_scratch_release(scratch);
        }
        return;
    }

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy, in_elev, el);
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

/* ------------------------------------------------------------------------- */
/* frame                                                                     */
/* ------------------------------------------------------------------------- */

/* ---- promoted subtrees: layers the compositor moves (scene_node.promoted) -- */

/* all a subtree stains: every node's footprint, shadows included */
static struct frect subtree_footprint(struct scene_arena *a, struct node_handle h, const float wparent[16]) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || g_sb_depth > 1024) return frect_empty();
    g_sb_depth++;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct frect b = node_footprint(n, node_world_aabb(n, world));
    struct node_handle c = n->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        if (!nested_layer(cn)) b = frect_union(b, subtree_footprint(a, c, world));
        c = next;
    }
    g_sb_depth--;
    return b;
}
/* the renderer clears what it gathered; a promoted subtree is not gathered */
static void subtree_clean(struct scene_arena *a, struct node_handle h, int depth) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || depth > 1024) return;
    n->dirty = 0; n->dirty_content = 0; n->dirty_look = 0; n->dirty_comp = 0;
    if (n->kind == SCENE_NODE_IMAGE) n->data.image.damage = 0;
    struct node_handle c = n->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        if (!nested_layer(cn)) subtree_clean(a, c, depth + 1);   /* its own pass cleans it */
        c = next;
    }
}

static struct scene_promoted *promoted_find(struct scene_renderer *r, struct node_handle h) {
    for (int i = 0; i < SCENE_LAYERS; i++)
        if (r->promoted[i].id && r->promoted[i].index == h.index && r->promoted[i].generation == h.generation)
            return &r->promoted[i];
    return 0;
}
static int clamp255(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f); }

/* where the subtree goes at one end of its motion: from where it rests */
static struct scene_layer_state promoted_end(const struct scene_layer_state *at, const struct scene_layer_motion *m,
                                             struct frect node, int bx, int by, int bw, int bh) {
    struct scene_layer_state e = *at;
    e.opacity = (uint8_t)((uint32_t)m->opacity * at->opacity / 255u);
    e.scale_milli = m->scale_milli ? m->scale_milli : 1000;
    /* the anchor is the node's own, and the layer is its footprint: map it */
    float axw = node.x + node.w * (float)m->anchor_x_milli / 1000.0f;
    float ayw = node.y + node.h * (float)m->anchor_y_milli / 1000.0f;
    e.anchor_x_milli = (uint16_t)(bw > 0 ? fminf(1000.0f, fmaxf(0.0f, (axw - (float)bx) * 1000.0f / (float)bw)) : 500);
    e.anchor_y_milli = (uint16_t)(bh > 0 ? fminf(1000.0f, fmaxf(0.0f, (ayw - (float)by) * 1000.0f / (float)bh)) : 500);
    e.x += m->dx; e.y += m->dy;
    return e;
}

/* ---- a promoted subtree's own damage: what changed inside it ---------- */

/* each node's footprint in the layer's pixels, walk order (as group_changed's) */
static int g_pr_depth;
static void promoted_feet(struct scene_arena *a, struct node_handle h, const float wparent[16], float ox, float oy,
                          struct scene_promoted_node *out, uint32_t cap, uint32_t *n, int *dirty_comp,
                          int *dirty_idx, int *n_dirty, int ndmax) {
    struct scene_node *nd = scene_resolve(a, h);
    if (!nd || g_pr_depth > 1024) return;
    float local[16], world[16];
    scene_trs_to_matrix(nd, local);
    scene_mat4_mul(wparent, local, world);
    struct frect f = node_footprint(nd, node_world_aabb(nd, world));
    if (*n < cap) {
        out[*n].x = f.x - ox; out[*n].y = f.y - oy; out[*n].w = f.w; out[*n].h = f.h;
        if (nd->dirty && *n_dirty < ndmax) dirty_idx[(*n_dirty)++] = (int)*n;
        else if (nd->dirty) *n_dirty = ndmax + 1;        /* too many: paint it all */
    }
    (*n)++;
    if (nd->dirty_comp && *n > 1) *dirty_comp = 1;        /* a group inside changed how it composites */
    g_pr_depth++;
    struct node_handle c = nd->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        if (!nested_layer(cn)) promoted_feet(a, c, world, ox, oy, out, cap, n, dirty_comp, dirty_idx, n_dirty, ndmax);
        c = next;
    }
    g_pr_depth--;
}
/* a rect into a short list, merged into the one that grows least when full */
static void rect_merge_add(struct clip_rect *r, int *n, int max, struct scene_promoted_node f) {
    float x0 = floorf(f.x), y0 = floorf(f.y), x1 = ceilf(f.x + f.w), y1 = ceilf(f.y + f.h);
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < *n; i++)                       /* already covered */
        if (x0 >= r[i].x && y0 >= r[i].y && x1 <= r[i].x + r[i].w && y1 <= r[i].y + r[i].h) return;
    if (*n < max) { r[*n] = (struct clip_rect){ x0, y0, x1 - x0, y1 - y0, 0 }; (*n)++; return; }
    int best = 0; float grow = 0;
    for (int i = 0; i < *n; i++) {
        float ux0 = fminf(r[i].x, x0), uy0 = fminf(r[i].y, y0);
        float ux1 = fmaxf(r[i].x + r[i].w, x1), uy1 = fmaxf(r[i].y + r[i].h, y1);
        float g = (ux1 - ux0) * (uy1 - uy0) - r[i].w * r[i].h;
        if (i == 0 || g < grow) { grow = g; best = i; }
    }
    float ux0 = fminf(r[best].x, x0), uy0 = fminf(r[best].y, y0);
    float ux1 = fmaxf(r[best].x + r[best].w, x1), uy1 = fmaxf(r[best].y + r[best].h, y1);
    r[best] = (struct clip_rect){ ux0, uy0, ux1 - ux0, uy1 - uy0, 0 };
}

/* does anything inside it paint into its pixels? (a nested layer does not) */
static int paints_children(struct scene_arena *a, const struct scene_node *n) {
    struct node_handle c = n->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        if (!cn) return 1;
        if (!nested_layer(cn)) return 1;
        c = cn->next_sibling;
    }
    return 0;
}

/* 0: no layer yet; -1: none (it shows nothing); 1: its layer; 2: its layer,
 * made this frame. `hold`: the
 * layer round it is not made yet, and this one may not be made before it (a
 * window's layers stack in the order they were made). `reborn`: the one round
 * it was made again, so this one is too, above it, where it stood. */
static int promoted_layer(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                          struct scene_node *n, const float wparent[16], struct render_target *target,
                          int hold, int reborn) {
    const struct scene_layer_host *host = r->host;
    struct frect b = subtree_footprint(a, h, wparent);
    int bx = (int)floorf(b.x), by = (int)floorf(b.y);
    int bw = (int)ceilf(b.x + b.w) - bx, bh = (int)ceilf(b.y + b.h) - by;
    if (bw <= 0 || bh <= 0) return -1;                 /* nothing to wait for: those inside go on */
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct frect nb = node_world_aabb(n, world);
    uint32_t sig = 0; int blur = 0;
    int changed = group_changed(a, h, &sig, &blur);
    float fx = b.x - (float)bx, fy = b.y - (float)by;

    struct scene_promoted *p = promoted_find(r, h);
    int fresh = 0, quiet = 0;
#ifdef PROMOTE_TRACE
    fprintf(stderr, "promote: node %u seen p=%d %dx%d changed=%d\n", h.index, p != 0, bw, bh, changed);
#endif
    if (p && (reborn || p->w != (uint32_t)bw || p->h != (uint32_t)bh)) {
        /* another size, or above a layer made again: another layer, standing
         * where the old one stood -- made now, with no arrival */
        host->go(host->ctx, p->id, 0, &p->at, 0, 0, 0, 1, 0);
        free(p->nodes);
        memset(p, 0, sizeof *p); p = 0; quiet = 1;
        for (int i = 0; i < SCENE_LAYERS && !p; i++) if (!r->promoted[i].id && !r->promoted[i].waiting) p = &r->promoted[i];
        if (!p) return 0;
    }
    if (!p || quiet) {
        /* SETTLED FIRST. A subtree's place can lag its first frame by one --
         * a menu measures where it opens against the frame before -- and a
         * layer sent in from the wrong place, then told where it really is,
         * would jump there and lose its arrival. So it waits, unpainted, until
         * it stands in one place two frames running (three at most), and asks
         * for the frame that tells. */
        /* ALREADY ON SCREEN -- the window's content, promoted for a pull --
         * arrives from where it is: no motion, so no settling to wait for */
        const struct scene_layer_motion *ap = &n->appear;
        if (ap->opacity == 255 && (ap->scale_milli == 0 || ap->scale_milli == 1000) && !ap->dx && !ap->dy && !hold) {
            if (!p) for (int i = 0; i < SCENE_LAYERS && !p; i++)
                if (r->promoted[i].waiting && r->promoted[i].index == h.index && r->promoted[i].generation == h.generation)
                    p = &r->promoted[i];
            if (!p) for (int i = 0; i < SCENE_LAYERS && !p; i++) if (!r->promoted[i].id && !r->promoted[i].waiting) p = &r->promoted[i];
            if (!p) return 0;
            memset(p, 0, sizeof *p); quiet = 1;
        }
        struct scene_promoted *w = quiet ? p : 0;
        for (int i = 0; i < SCENE_LAYERS && !w; i++)
            if (r->promoted[i].waiting && r->promoted[i].index == h.index && r->promoted[i].generation == h.generation)
                w = &r->promoted[i];
        if (!w) {
            for (int i = 0; i < SCENE_LAYERS && !w; i++) if (!r->promoted[i].id && !r->promoted[i].waiting) w = &r->promoted[i];
            if (!w) return 0;
            memset(w, 0, sizeof *w);
            w->index = h.index; w->generation = h.generation; w->waiting = 1;
            w->at.x = bx; w->at.y = by; w->w = (uint32_t)bw; w->h = (uint32_t)bh;
            w->seen = 1; r->wants_frame = 1;
            return 0;
        }
        w->seen = 1;
        if (!quiet && (((w->at.x != bx || w->at.y != by || w->w != (uint32_t)bw || w->h != (uint32_t)bh) &&
                        w->waiting < 3) || hold)) {
            w->at.x = bx; w->at.y = by; w->w = (uint32_t)bw; w->h = (uint32_t)bh;
            if (w->waiting < 3) w->waiting++;
            r->wants_frame = 1;
            return 0;
        }
        p = w; p->waiting = 0;
        /* ONE COLOUR, a box and nothing else -- a dimming -- is a fill: a
         * 1 x 1 layer that stands as large as the box, not its pixels */
        int fill = host->look && n->kind == SCENE_NODE_RECT && !paints_children(a, n) &&
                   n->data.rect.fill.kind == PAINT_SOLID && n->corner_radius <= 0 && !n->shadow_enabled &&
                   n->border_width <= 0 && !n->backdrop_blur_enabled;
        uint32_t *px = 0;
        uint32_t id = host->create(host->ctx, fill ? 1u : (uint32_t)bw, fill ? 1u : (uint32_t)bh, &px);
        if (!id || !px) { memset(p, 0, sizeof *p); return 0; }
        p->id = id; p->px = px; p->w = (uint32_t)bw; p->h = (uint32_t)bh; p->fill = fill;
        p->index = h.index; p->generation = h.generation;
        fresh = 1;
        if (host->look) {            /* the glass is the node's own box, not the shadow round it */
            if (fill) host->look(host->ctx, id, 0, 0, 0, 0, bw, bh, 1);
            else host->look(host->ctx, id, (uint8_t)fminf(255.0f, n->corner_radius), n->backdrop_blur_enabled,
                            (int)floorf(nb.x) - bx, (int)floorf(nb.y) - by, (int)ceilf(nb.w), (int)ceilf(nb.h), 0);
        }
    }
    p->seen = 1;

    struct scene_layer_state at = { bx, by, (uint8_t)clamp255(n->opacity), 1000, 500, 500 };
    if (n->group_scale < 1.0f) {                       /* a scale the app set itself */
        at.scale_milli = (uint16_t)(n->group_scale * 1000.0f + 0.5f);
        at.anchor_x_milli = (uint16_t)fminf(1000.0f, fmaxf(0.0f, (nb.x + nb.w * n->anchor_x - (float)bx) * 1000.0f / (float)bw));
        at.anchor_y_milli = (uint16_t)fminf(1000.0f, fmaxf(0.0f, (nb.y + nb.h * n->anchor_y - (float)by) * 1000.0f / (float)bh));
    }
    /* repaint its pixels when anything in it changed -- or, for glass, when
     * what is behind it in the window did */
    int repaint = fresh || changed || p->sig != sig || fabsf(p->fx - fx) > 1e-3f || fabsf(p->fy - fy) > 1e-3f ||
                  p->reduced != g_effects_reduced || (blur && (r->full || dirty_hits(r, b)));
    /* WHAT CHANGED, inside: with the same nodes in it (the same shape), the
     * old and new places of each node that changed -- a hover moving down a
     * menu, a caret -- are all that is painted again. */
    struct clip_rect part[16]; int npart = -1;
    struct scene_promoted_node feet[512]; uint32_t nfeet = 0;
    int dirty_idx[32], n_dirty = 0, dcomp = 0;
    promoted_feet(a, h, wparent, (float)bx, (float)by, feet, 512, &nfeet, &dcomp, dirty_idx, &n_dirty, 32);
    if (p->fill && repaint) {
        /* a fill's one pixel: the colour, premultiplied */
        struct color c = n->data.rect.fill.solid;
        uint32_t A = (uint32_t)clamp255(c.a);
        p->px[0] = A << 24 | (uint32_t)clamp255(c.r * c.a) << 16 | (uint32_t)clamp255(c.g * c.a) << 8 |
                   (uint32_t)clamp255(c.b * c.a);
        p->sig = sig; p->fx = fx; p->fy = fy; p->reduced = g_effects_reduced;
        r->layer_paints++;
        npart = -2;                                     /* painted: no render */
    }
    if (repaint && npart != -2 && !fresh && p->nodes && p->n_nodes == nfeet && nfeet <= 512 && p->sig == sig &&
        fabsf(p->fx - fx) <= 1e-3f && fabsf(p->fy - fy) <= 1e-3f && p->reduced == g_effects_reduced &&
        !dcomp && n_dirty <= 32 && !(blur && !host->look && (r->full || dirty_hits(r, b)))) {
        npart = 0;
        for (uint32_t k = 0; k < nfeet; k++)
            if (feet[k].x != p->nodes[k].x || feet[k].y != p->nodes[k].y ||
                feet[k].w != p->nodes[k].w || feet[k].h != p->nodes[k].h) {
                rect_merge_add(part, &npart, 16, feet[k]); rect_merge_add(part, &npart, 16, p->nodes[k]);
            }
        for (int k = 0; k < n_dirty; k++) {
            rect_merge_add(part, &npart, 16, feet[dirty_idx[k]]);
            rect_merge_add(part, &npart, 16, p->nodes[dirty_idx[k]]);
        }
    }
    if (repaint && npart == 0) repaint = 0;            /* it changed nothing it shows */
    if (repaint && npart != -2) {
        struct render_target lt; memset(&lt, 0, sizeof lt);
        lt.pixels = p->px; lt.width = p->w; lt.height = p->h; lt.stride = p->w * 4;
        lt.format = EMBK_PIXFMT_BGRA8888_PRE;
        struct clip_saved sv;
        struct render_target *was_bd = r->backdrop; float wox = r->bd_ox, woy = r->bd_oy;
        r->backdrop = target; r->bd_ox = 0; r->bd_oy = 0;   /* glass in it frosts the window behind it */
        int was = r->no_cull; r->no_cull = 1;
        if (host->look) r->glass_root = n;              /* its own glass is the compositor's */
        cpu_clip_save_and_clear(&sv);
        if (npart > 0) {
            /* only those rects: cleared, and painted again -- clipped to them */
            lt.clear_dirty = 1;
            r->be->begin_frame(&lt, part, (uint32_t)npart);
        } else {
            memset(p->px, 0, (size_t)p->w * p->h * 4);
        }
        render_subtree_opaque(r, a, h, wparent, &lt, (float)bx, (float)by);
        if (npart > 0) r->be->end_frame(&lt);
        cpu_clip_restore(&sv);
        r->glass_root = 0;
        r->no_cull = was; r->backdrop = was_bd; r->bd_ox = wox; r->bd_oy = woy;
        p->sig = sig; p->fx = fx; p->fy = fy; p->reduced = g_effects_reduced;
        if (npart > 0) r->layer_partials++; else r->layer_paints++;
    }
    /* where its nodes are now, for the next change */
    if (nfeet <= 512) {
        if (p->cap_nodes < nfeet) {
            struct scene_promoted_node *nn = (struct scene_promoted_node *)realloc(p->nodes, sizeof *nn * (nfeet ? nfeet : 1));
            if (nn) { p->nodes = nn; p->cap_nodes = nfeet; }
        }
        if (p->cap_nodes >= nfeet) { memcpy(p->nodes, feet, sizeof *feet * nfeet); p->n_nodes = nfeet; }
    } else p->n_nodes = 0;
#ifdef PROMOTE_TRACE
    fprintf(stderr, "promote: id %u fresh=%d quiet=%d repaint=%d at %d,%d op=%d appear op=%d resp=%d\n", p->id, fresh, quiet, repaint, at.x, at.y, at.opacity, n->appear.opacity, n->appear.response_ms);
#endif
    if (fresh) {
        struct scene_layer_state from = promoted_end(&at, &n->appear, nb, bx, by, bw, bh);
        at.anchor_x_milli = from.anchor_x_milli; at.anchor_y_milli = from.anchor_y_milli;
        host->go(host->ctx, p->id, quiet ? 0 : &from, &at, n->appear.response_ms, n->appear.damping_milli,
                 quiet ? 0 : n->appear.delay_ms, 0, 1);
    } else if (memcmp(&at, &p->at, sizeof at)) {
        if (at.scale_milli == 1000) { at.anchor_x_milli = p->at.anchor_x_milli; at.anchor_y_milli = p->at.anchor_y_milli; }
        if (memcmp(&at, &p->at, sizeof at))
            /* the app moved it: it is there -- or on its way, on the spring it said */
            host->go(host->ctx, p->id, 0, &at, n->layer_move_ms, n->layer_move_damping, 0, 0, repaint);
    }
    if (repaint && npart > 0) {
        for (int k = 0; k < npart; k++)
            host->present(host->ctx, p->id, (int)part[k].x, (int)part[k].y, (int)part[k].w, (int)part[k].h);
    } else if (repaint) host->present(host->ctx, p->id, 0, 0, p->fill ? 1 : bw, p->fill ? 1 : bh);
    p->at = at;
    p->disappear = n->disappear;        /* where it goes when it is gone... */
    p->nx = nb.x; p->ny = nb.y; p->nw = nb.w; p->nh = nb.h;   /* ...about the node as it is now */
    subtree_clean(a, h, 0);
    return fresh ? 2 : 1;
}

static void promoted_walk(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                          const float wparent[16], struct render_target *target, int depth,
                          int hold, int reborn) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || depth > 1024) return;
    if (n->promoted) {                                  /* ...and then the layers inside it */
        int st = promoted_layer(r, a, h, n, wparent, target, hold, reborn);
        hold = hold || st == 0; reborn = st == 2;
    }
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct node_handle c = n->first_child;
    uint32_t guard = 512;
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        promoted_walk(r, a, c, world, target, depth + 1, hold, reborn);
        c = next;
    }
}

/* a promoted subtree that is gone: it leaves as it said it would, and the
 * host lets the layer go once it is there */
static void promoted_leave(struct scene_renderer *r, struct scene_promoted *p) {
    const struct scene_layer_host *host = r->host;
#ifdef PROMOTE_TRACE
    fprintf(stderr, "promote: id %u leaves\n", p->id);
#endif
    struct frect nb = { p->nx, p->ny, p->nw, p->nh };
    struct scene_layer_state to = promoted_end(&p->at, &p->disappear, nb, p->at.x, p->at.y, (int)p->w, (int)p->h);
    if (host) host->go(host->ctx, p->id, 0, &to, p->disappear.response_ms, p->disappear.damping_milli,
                       p->disappear.delay_ms, 1, 0);
    free(p->nodes);
    memset(p, 0, sizeof *p);
}

void scene_render_after_present(struct scene_renderer *r) {
    for (int i = 0; i < r->n_doomed; i++)
        if (r->host) r->host->go(r->host->ctx, r->doomed[i].id, 0, &r->doomed[i].at, 0, 0, 0, 1, 0);
    r->n_doomed = 0;
}

void scene_render_set_layer_host(struct scene_renderer *r, const struct scene_layer_host *host) {
    scene_render_after_present(r);
    if (r->host == host) return;
    for (int i = 0; i < SCENE_LAYERS; i++) if (r->promoted[i].id) promoted_leave(r, &r->promoted[i]);
    r->host = host;
}

void scene_render_init(struct scene_renderer *r, struct render_backend *be) {
    r->be = be; r->cache = 0; r->cache_cap = 0; r->n_dirty = 0; r->full = 0; r->has_scroll_present = 0;
    r->backdrop = 0; r->bd_ox = r->bd_oy = 0;
    r->no_cull = 0; r->group_hits = r->group_paints = 0;
    memset(r->groups, 0, sizeof r->groups);
    r->host = 0; memset(r->promoted, 0, sizeof r->promoted); r->layer_paints = 0; r->wants_frame = 0;
    r->n_doomed = 0;
}
void scene_render_destroy(struct scene_renderer *r) {
    scene_render_after_present(r);
    for (int i = 0; i < SCENE_LAYERS; i++) if (r->promoted[i].id) promoted_leave(r, &r->promoted[i]);
    free(r->cache); r->cache = 0; r->cache_cap = 0;
    for (int i = 0; i < SCENE_GROUP_CACHE; i++) group_drop(&r->groups[i]);
}

int scene_render_set_reduced(int on) {
    on = !!on;
    if (on == g_effects_reduced) return 0;
    g_effects_reduced = on;
    return 1;
}

void scene_render_invalidate(struct scene_renderer *r) {
    if (!r) return;
    for (int i = 0; i < SCENE_GROUP_CACHE; i++) group_drop(&r->groups[i]);
    for (int i = 0; i < SCENE_LAYERS; i++) r->promoted[i].sig = 0;   /* their pixels, painted again */
    if (!r->cache) return;
    for (uint32_t i = 0; i < r->cache_cap; i++) r->cache[i].valid = 0;
}

static void ensure_cache(struct scene_renderer *r, uint32_t need) {
    if (r->cache_cap >= need) return;
    uint32_t cap = r->cache_cap ? r->cache_cap : 64;
    while (cap < need) cap *= 2;
    struct rect_cache *nc = (struct rect_cache *)realloc(r->cache, sizeof(*nc) * cap);
    if (!nc) return;
    for (uint32_t i = r->cache_cap; i < cap; i++) { nc[i].valid = 0; nc[i].generation = 0; }
    r->cache = nc; r->cache_cap = cap;
}

void scene_render_frame(struct scene_renderer *r, struct scene_arena *a,
                        struct node_handle root, struct render_target *target) {
    r->n_dirty = 0; r->full = 0; r->has_scroll_present = 0;
    ensure_cache(r, a->next_never_used);
    if (!r->cache) return;

    /* gather every node once, in paint order, with world AABB + group context */
    uint32_t cap = a->next_never_used;
    struct noderec *recs = (struct noderec *)malloc(sizeof(struct noderec) * (cap ? cap : 1));
    if (!recs) return;
    int nrec = 0;
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    g_promote_live = r->host != 0;
    gather(a, root, ident, 0, frect_empty(), 0, frect_empty(), recs, &nrec, (int)cap);

    /* --- Step 1: dirty accumulation (own dirty flag OR world rect moved OR
     * newly created); union BOTH the new footprint and the cached old one so a
     * moved element doesn't leave a ghost at its previous location. The cache
     * stores the shadow-grown FOOTPRINT, not the bare AABB -- a ghost occupies
     * everything the node stained, and a drop shadow stains past the box. --- */
    uint8_t *seen = (uint8_t *)calloc(r->cache_cap ? r->cache_cap : 1, 1);
    /* --- Step 1a: CLASSIFY, adding nothing yet. Whether this frame is "a
     * scroll" is only knowable from the whole population of changes, so the
     * verdicts are gathered before any dirty rect is committed. --- */
    int n_moved = 0, n_content = 0, uniform = 1;
    float mdx = 0, mdy = 0;
    for (int i = 0; i < nrec; i++) {
        struct noderec *rec = &recs[i];
        uint32_t idx = rec->h.index;
        struct scene_node *n = scene_resolve(a, rec->h);
        struct rect_cache *cc = (idx < r->cache_cap) ? &r->cache[idx] : 0;
        int cache_live = cc && cc->valid && cc->generation == rec->h.generation;
        if (seen && idx < r->cache_cap && cache_live) seen[idx] = 1;
        int moved = 1;
        if (cache_live) {
            rec->old.x = cc->x; rec->old.y = cc->y; rec->old.w = cc->w; rec->old.h = cc->h;
            rec->has_old = 1;
            moved = !(fabsf(cc->x - rec->foot.x) < 0.01f && fabsf(cc->y - rec->foot.y) < 0.01f &&
                      fabsf(cc->w - rec->foot.w) < 0.01f && fabsf(cc->h - rec->foot.h) < 0.01f);
        }
        /* own_dirty means CONTENT changed -- a pure transform is a move, and
         * moves are what the scroll blit exists for (scene.h dirty_content) */
        rec->own_dirty = n && n->dirty_content;
        int any_dirty = n && n->dirty;
        (void)any_dirty;
        rec->moved = moved;
        rec->dirty = rec->own_dirty || moved;
        if (rec->own_dirty) {
            n_content++;
#ifdef SCROLL_DEBUG
            fprintf(stderr, "content: OWN-DIRTY kind=%d at %.0f,%.0f %.0fx%.0f\n",
                    n ? (int)n->kind : -1, rec->foot.x, rec->foot.y, rec->foot.w, rec->foot.h);
#endif
        }
        if (moved && !cache_live) {
            n_content++;            /* a NEW node is content */
#ifdef SCROLL_DEBUG
            fprintf(stderr, "content: NEW kind=%d at %.0f,%.0f %.0fx%.0f\n",
                    n ? (int)n->kind : -1, rec->foot.x, rec->foot.y, rec->foot.w, rec->foot.h);
#endif
        }
        if (moved && cache_live) {
            /* a translation keeps its size; a resize is content */
            if (fabsf(rec->old.w - rec->foot.w) > 0.01f ||
                fabsf(rec->old.h - rec->foot.h) > 0.01f) {
                n_content++;
#ifdef SCROLL_DEBUG
                fprintf(stderr, "content: RESIZE kind=%d %.0fx%.0f -> %.0fx%.0f\n",
                        n ? (int)n->kind : -1, rec->old.w, rec->old.h, rec->foot.w, rec->foot.h);
#endif
            }
            else {
                rec->trans = 1;
                rec->tdx = rec->foot.x - rec->old.x;
                rec->tdy = rec->foot.y - rec->old.y;
            }
        }
    }
    /* THE SCROLL'S DELTA IS THE ONE MOST OF THE MOVES SHARE. A scroll view
     * that overflows shows a bar, and the bar's thumb moves the other way, a
     * fraction as far: insisting that EVERY move be the same one meant the
     * thumb alone vetoed the blit for every page with a scroll bar -- which is
     * every page the blit exists for. The other moves are the frame's own
     * business: they are held to what content changes are held to below
     * (outside the blitted region, repainted as ordinary rects). */
    {
        struct { float dx, dy; int n; } bk[8];
        int nb = 0;
        for (int i = 0; i < nrec && uniform; i++) {
            if (!recs[i].trans) continue;
            int b = 0;
            while (b < nb && (fabsf(recs[i].tdx - bk[b].dx) > 0.5f || fabsf(recs[i].tdy - bk[b].dy) > 0.5f)) b++;
            if (b == nb) {
                if (nb == 8) { uniform = 0; break; }   /* no one delta to speak of */
                bk[nb].dx = recs[i].tdx; bk[nb].dy = recs[i].tdy; bk[nb].n = 0; nb++;
            }
            bk[b].n++;
        }
        int best = -1;
        for (int b = 0; uniform && b < nb; b++) if (best < 0 || bk[b].n > bk[best].n) best = b;
        if (best >= 0) {
            mdx = bk[best].dx; mdy = bk[best].dy; n_moved = bk[best].n;
            for (int i = 0; i < nrec; i++)
                if (recs[i].trans && (fabsf(recs[i].tdx - mdx) > 0.5f || fabsf(recs[i].tdy - mdy) > 0.5f))
                    recs[i].odd = 1;
        }
    }
    int n_vacated = 0;
    for (uint32_t i = 0; i < r->cache_cap; i++)
        if (r->cache[i].valid && seen && !seen[i]) n_vacated++;

    /* --- Step 1s: THE SCROLL BLIT. When many nodes translated by one shared
     * whole-pixel vertical delta and nothing INSIDE the scrolled clip did
     * anything else (a thumb beside it moving its own way is fine -- Step 1a
     * calls that an odd move and it repaints as content), this frame is a
     * scroll -- and a
     * scroll does not need its pixels recomputed, it needs them MOVED. The
     * old frame already contains almost every pixel of the new one, |dy|
     * higher or lower. So: find the clip container being scrolled (the
     * smallest clipping node intersecting the motion), memmove its rows, and
     * repaint only the strip the motion exposed.
     *
     * Without this, a wheel tick moved everything in the viewport, blew the
     * 16-rect dirty cap, and degraded to a FULL repaint -- hundreds of glyph
     * blits per notch, which under TCG is a browser that scrolls in lurches.
     * With it, a notch costs one memmove and a strip of glyphs.
     *
     * The guards are the point. Any content change, any resize, any vacated
     * node, any moved-but-visible node OUTSIDE the clip aborts to the normal
     * path -- correctness first, the blit only when the frame provably is a
     * pure scroll. */
    int scrolled = 0;
#ifdef SCROLL_DEBUG
    fprintf(stderr, "scroll? full=%d uniform=%d moved=%d content=%d vacated=%d d=(%.1f,%.1f)\n",
            r->full, uniform, n_moved, n_content, n_vacated, mdx, mdy);
#endif
    /* Content changes do not veto the scroll GLOBALLY -- only content INSIDE
     * the scrolled region does. A status line ticking at the bottom of the
     * window, or a hover highlight in the toolbar, has nothing to do with the
     * document scrolling above it; those repaint as ordinary dirty rects
     * alongside the strip. The intersection check happens after R is known. */
    (void)n_content;
    if (!r->full && uniform && n_moved >= 8 &&
        fabsf(mdx) < 0.5f && fabsf(mdy) >= 1.0f &&
        /* a copy moves WHOLE rows: a fractional move resamples, and copying
         * it by the rounded amount leaves everything in the view off by the
         * difference (ui_set_scroll_offset shifts by whole pixels for this) */
        fabsf(mdy - (float)(int)(mdy < 0 ? mdy - 0.5f : mdy + 0.5f)) < 0.02f &&
        target && target->pixels) {
        /* the union of the moving content, old and new */
        struct frect mb; int have_mb = 0;
        for (int i = 0; i < nrec; i++) {
            if (!(recs[i].moved && recs[i].has_old) || recs[i].odd) continue;
            struct frect u = recs[i].foot;
            if (!have_mb) { mb = u; have_mb = 1; }
            float x1 = mb.x + mb.w, y1 = mb.y + mb.h;
            if (u.x < mb.x) mb.x = u.x;
            if (u.y < mb.y) mb.y = u.y;
            if (u.x + u.w > x1) x1 = u.x + u.w;
            if (u.y + u.h > y1) y1 = u.y + u.h;
            mb.w = x1 - mb.x; mb.h = y1 - mb.y;
        }
        /* the scrolled clip: smallest UNMOVED clipping node intersecting it */
        int ci = -1; float carea = 0;
        for (int i = 0; i < nrec; i++) {
            if (!recs[i].clips || recs[i].moved || recs[i].own_dirty) continue;
            if (!have_mb || !frect_overlap(recs[i].aabb, mb)) continue;
            float ar = recs[i].aabb.w * recs[i].aabb.h;
            if (ci < 0 || ar < carea) { ci = i; carea = ar; }
        }
#ifdef SCROLL_DEBUG
        fprintf(stderr, "scroll? clip ci=%d\n", ci);
#endif
        if (ci >= 0) {
            struct frect R = recs[ci].aabb;
            /* clamp to the target */
            if (R.x < 0) { R.w += R.x; R.x = 0; }
            if (R.y < 0) { R.h += R.y; R.y = 0; }
            if (R.x + R.w > (float)target->width)  R.w = (float)target->width  - R.x;
            if (R.y + R.h > (float)target->height) R.h = (float)target->height - R.y;
            /* every UNIFORMLY-MOVED node must be inside R or off-target
             * (moved content visible outside the clip means this was not that
             * clip's scroll); and every CONTENT change and VACATED slot must be
             * OUTSIDE R (stale pixels under the blit would be smeared). */
            int ok = R.w > 0 && R.h > 0;
            for (int i = 0; ok && i < nrec; i++) {
                struct frect t = { 0, 0, (float)target->width, (float)target->height };
                int resized = recs[i].moved && recs[i].has_old &&
                              (fabsf(recs[i].old.w - recs[i].foot.w) > 0.01f ||
                               fabsf(recs[i].old.h - recs[i].foot.h) > 0.01f);
                int is_content = recs[i].own_dirty || resized || recs[i].odd ||
                                 (recs[i].moved && !recs[i].has_old);
                /* VISIBLE extents, not raw footprints: a node scrolled past the
                 * bottom of its container stains nothing, so it can neither
                 * smear the blit nor prove the frame was not a scroll. Using
                 * the footprint meant any document taller than its viewport
                 * vetoed its own fast path -- which is every real page. */
                struct frect vnew = recs[i].vis;
                struct frect vold = recs[i].has_old
                    ? (recs[i].has_clipbox ? frect_isect(recs[i].old, recs[i].clipbox)
                                           : recs[i].old)
                    : frect_empty();
                if (is_content) {
                    if (frect_overlap(vnew, recs[ci].aabb) ||
                        frect_overlap(vold, recs[ci].aabb) ||
                        (recs[i].has_extent && frect_overlap(recs[i].extent, recs[ci].aabb))) {
                        ok = 0;
#ifdef SCROLL_DEBUG
                        struct scene_node *vn = scene_resolve(a, recs[i].h);
                        fprintf(stderr, "veto: CONTENT-INSIDE kind=%d own=%d new=%d resized=%d %.0f,%.0f %.0fx%.0f\n",
                                vn ? (int)vn->kind : -1, recs[i].own_dirty,
                                recs[i].moved && !recs[i].has_old, resized,
                                recs[i].foot.x, recs[i].foot.y, recs[i].foot.w, recs[i].foot.h);
#endif
                    }
                    continue;
                }
                if (!recs[i].moved) continue;
                /* A moved node that CLIPS invalidates the reasoning above: its
                 * descendants' visible extents were computed against a clip
                 * that is itself in motion. Rare (a scrollable inside a
                 * scrollable) and cheap to refuse. */
                if (recs[i].clips) { ok = 0;
#ifdef SCROLL_DEBUG
                    fprintf(stderr, "veto: MOVED-CLIP\n");
#endif
                    continue; }
                if (!frect_overlap(vnew, t) && !frect_overlap(vold, t)) continue;
                struct frect both = vnew;
                if (!frect_is_empty(vold)) {
                    if (frect_is_empty(both)) both = vold;
                    else {
                        float x1 = both.x + both.w, y1 = both.y + both.h;
                        if (vold.x < both.x) both.x = vold.x;
                        if (vold.y < both.y) both.y = vold.y;
                        if (vold.x + vold.w > x1) x1 = vold.x + vold.w;
                        if (vold.y + vold.h > y1) y1 = vold.y + vold.h;
                        both.w = x1 - both.x; both.h = y1 - both.y;
                    }
                }
                if (!frect_overlap(both, recs[ci].aabb)) {
                    ok = 0;
#ifdef SCROLL_DEBUG
                    fprintf(stderr, "veto: MOVED-OUTSIDE %.0f,%.0f %.0fx%.0f\n",
                            both.x, both.y, both.w, both.h);
#endif
                }
            }
            for (uint32_t i = 0; ok && i < r->cache_cap; i++) {
                if (!r->cache[i].valid || (seen && seen[i])) continue;
                struct frect v = { r->cache[i].x, r->cache[i].y, r->cache[i].w, r->cache[i].h };
                if (frect_overlap(v, recs[ci].aabb)) ok = 0;   /* a ghost under the blit */
            }
#ifdef SCROLL_DEBUG
            fprintf(stderr, "scroll? ok=%d R=%.0f,%.0f %.0fx%.0f\n", ok, R.x, R.y, R.w, R.h);
#endif
            if (ok) {
                /* Blit INTERIOR rows only. The clip's edges are fractional
                 * (95.6, say): the edge row blends clipped content with
                 * whatever sits outside, and copying an interior row over it
                 * stomps that blend -- one wrong antialiased row at the top of
                 * every scroll. ceil/floor keeps the blit inside, and the edge
                 * bands added below repaint the blends properly. */
                int rx0 = (int)ceilf(R.x),        ry0 = (int)ceilf(R.y);
                int rx1 = (int)floorf(R.x + R.w), ry1 = (int)floorf(R.y + R.h);
                int dy = (int)(mdy < 0 ? mdy - 0.5f : mdy + 0.5f);
                uint8_t *px = (uint8_t *)target->pixels;
                uint32_t stride = target->stride;
                size_t rowbytes = (size_t)(rx1 - rx0) * 4;
                if (dy < 0) {          /* content moved UP: copy top-down */
                    for (int y = ry0; y < ry1; y++) {
                        int sy = y - dy;
                        if (sy >= ry1) break;             /* rest is the exposed strip */
                        memmove(px + (size_t)y * stride + (size_t)rx0 * 4,
                                px + (size_t)sy * stride + (size_t)rx0 * 4, rowbytes);
                    }
                } else {               /* content moved DOWN: copy bottom-up */
                    for (int y = ry1 - 1; y >= ry0; y--) {
                        int sy = y - dy;
                        if (sy < ry0) break;
                        memmove(px + (size_t)y * stride + (size_t)rx0 * 4,
                                px + (size_t)sy * stride + (size_t)rx0 * 4, rowbytes);
                    }
                }
                /* the exposed strip, with 2px of slack for antialiased edges */
                struct frect strip;
                strip.x = R.x; strip.w = R.w;
                if (dy < 0) { strip.y = (float)(ry1 + dy - 2); strip.h = (float)(-dy + 4); }
                else        { strip.y = (float)(ry0 - 2);      strip.h = (float)(dy + 4); }
                dirty_add(r, strip);
                /* the fractional edge rows of R: repaint their blends */
                { struct frect e1 = { R.x, R.y - 1.0f, R.w, 3.0f };
                  struct frect e2 = { R.x, R.y + R.h - 2.0f, R.w, 3.0f };
                  dirty_add(r, e1); dirty_add(r, e2); }
                /* and its ROUNDED CORNERS: there the clip's own coverage
                 * depends on the height, so a pixel copied from a few rows
                 * away carries the wrong amount of the clip with it */
                { struct scene_node *cn = scene_resolve(a, recs[ci].h);
                  float cr = cn ? cn->corner_radius : 0.0f;
                  if (cr > 0.0f) {
                      float k = cr + 2.0f;
                      struct frect c4[4] = {
                          { R.x - 1.0f,         R.y - 1.0f,         k, k },
                          { R.x + R.w + 1.0f - k, R.y - 1.0f,       k, k },
                          { R.x - 1.0f,         R.y + R.h + 1.0f - k, k, k },
                          { R.x + R.w + 1.0f - k, R.y + R.h + 1.0f - k, k, k } };
                      for (int q = 0; q < 4; q++) dirty_add(r, c4[q]);
                  } }
                /* content changes OUTSIDE R repaint as ordinary dirty rects */
                for (int i = 0; i < nrec; i++) {
                    int resized = recs[i].moved && recs[i].has_old &&
                                  (fabsf(recs[i].old.w - recs[i].foot.w) > 0.01f ||
                                   fabsf(recs[i].old.h - recs[i].foot.h) > 0.01f);
                    int is_content = recs[i].own_dirty || resized || recs[i].odd ||
                                     (recs[i].moved && !recs[i].has_old);
                    if (!is_content) continue;
                    dirty_add(r, recs[i].foot);
                    if (recs[i].has_extent) dirty_add(r, recs[i].extent);
                    if (recs[i].has_old) dirty_add(r, recs[i].old);
                }
                scrolled = 1;
                r->sp_x = R.x; r->sp_y = R.y; r->sp_w = R.w; r->sp_h = R.h;
                r->has_scroll_present = 1;  /* the consumer must present ALL of R */
            }
        }
    }

    /* --- Step 1c: the normal dirty accumulation, skipped when the frame was
     * resolved as a scroll (the strip is the only repaint). --- */
    if (!scrolled) {
        for (int i = 0; i < nrec; i++) {
            struct noderec *rec = &recs[i];
            if (!rec->dirty) continue;
            /* an image that said which part of it changed, in place: that part */
            struct scene_node *n = scene_resolve(a, rec->h);
            if (n && n->kind == SCENE_NODE_IMAGE && n->data.image.damage == 1 && !rec->moved &&
                rec->has_old && n->data.image.w && n->data.image.h) {
                float sx = rec->aabb.w / (float)n->data.image.w, sy = rec->aabb.h / (float)n->data.image.h;
                struct frect d = { rec->aabb.x + n->data.image.dmg_x * sx, rec->aabb.y + n->data.image.dmg_y * sy,
                                   n->data.image.dmg_w * sx, n->data.image.dmg_h * sy };
                dirty_add(r, d);
                continue;
            }
            dirty_add(r, rec->foot);                                  /* new footprint */
            if (rec->has_extent) dirty_add(r, rec->extent);           /* a group recomposited */
            if (rec->has_old) dirty_add(r, rec->old);                 /* old ghost */
        }
    }

    /* --- Step 1b: the VACATED. A node that existed last frame and is gone this
     * frame -- destroyed, or its arena slot handed to a successor -- appears in
     * no rec, so nothing above ever mentions its pixels; they would sit on the
     * target forever, which is exactly the "closed launcher stays painted",
     * "removed icon lingers" family of bugs every app worked around with forced
     * full repaints. Its last painted footprint is right here in the cache:
     * dirty it, then drop the slot. Runs BEFORE the blur/group expansions so
     * they see these rects too. --- */
    for (uint32_t i = 0; i < r->cache_cap; i++) {
        struct rect_cache *cc = &r->cache[i];
        if (!cc->valid) continue;
        if (seen && i < r->cache_cap && seen[i]) continue;
        struct frect old = { cc->x, cc->y, cc->w, cc->h };
        dirty_add(r, old);
        cc->valid = 0;
    }
    free(seen);

    /* --- Step 2: backdrop-blur footprint expansion. Any dirty rect touching a
     * blur node forces a full back-to-front repaint of that whole region, so
     * the blur samples FRESH behind-content, not last frame's pixels. --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].is_blur && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].aabb);
    }

    /* --- Step 3: opacity-group atomicity. A dirty rect intersecting any node
     * inside an opacity<1 group promotes to that group's whole bounds (groups
     * re-render as a unit; no partial-scratch update). --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].has_group && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].group_bounds);
    }

    /* --- Step 4: cap already collapses to full-screen in dirty_add. Paint the
     * whole tree back-to-front; coverage_at() (backend) restricts actual pixel
     * writes to the dirty union, and render_node skips groups outside it. --- */
    int have_work = r->full || r->n_dirty > 0;
    if (have_work) {
        if (r->full) {
            struct clip_rect fs = { 0, 0, (float)target->width, (float)target->height, 0 };
            r->be->begin_frame(target, &fs, 1);
        } else {
            r->be->begin_frame(target, r->dirty, (uint32_t)r->n_dirty);
        }
        struct elev_list el; el.n = 0;
        render_node(r, a, root, ident, target, 0.0f, 0.0f, 0, &el);
        /* deferred: ascending layer, stable within a layer (encounter order ==
         * document order). Insertion sort -- the list is tiny. */
        for (int i = 1; i < el.n; i++) {
            int j = i;
            while (j > 0 && el.e[j - 1].layer > el.e[j].layer) {
                struct elev_list tmp_one; tmp_one.e[0] = el.e[j - 1];
                el.e[j - 1] = el.e[j]; el.e[j] = tmp_one.e[0];
                j--;
            }
        }
        for (int i = 0; i < el.n; i++)
            render_node(r, a, el.e[i].h, el.e[i].wparent, target, 0.0f, 0.0f, 1, 0);
        r->be->end_frame(target);
        /* a group not met this frame is gone (or no longer a group): let its
         * pixels go */
        for (int i = 0; i < SCENE_GROUP_CACHE; i++) {
            if (r->groups[i].index && !r->groups[i].used) group_drop(&r->groups[i]);
            r->groups[i].used = 0;
        }
    }

    /* update cache to this frame's rects + clear consumed dirty flags */
    for (int i = 0; i < nrec; i++) {
        uint32_t idx = recs[i].h.index;
        if (idx < r->cache_cap) {
            r->cache[idx].x = recs[i].foot.x; r->cache[idx].y = recs[i].foot.y;
            r->cache[idx].w = recs[i].foot.w; r->cache[idx].h = recs[i].foot.h;
            r->cache[idx].generation = recs[i].h.generation;
            r->cache[idx].valid = 1;
        }
        struct scene_node *n = scene_resolve(a, recs[i].h);
        if (n) { n->dirty = 0; n->dirty_content = 0; n->dirty_look = 0; n->dirty_comp = 0; if (n->kind == SCENE_NODE_IMAGE) n->data.image.damage = 0; }
    }
    free(recs);

    /* THE PROMOTED PASS: each promoted subtree to its layer -- painted when it
     * changed, moved when its place did, sent away when it is gone. Every
     * frame, after the window's own pixels, which its glass is frosted from. */
    if (r->host) {
        scene_render_after_present(r);                  /* last frame's, if the runtime did not */
        r->wants_frame = 0;
        for (int i = 0; i < SCENE_LAYERS; i++) r->promoted[i].seen = 0;
        promoted_walk(r, a, root, ident, target, 0, 0, 0);
        for (int i = 0; i < SCENE_LAYERS; i++) {
            struct scene_promoted *p = &r->promoted[i];
            if (p->id && !p->seen && scene_resolve(a, (struct node_handle){ p->index, p->generation })) {
                /* STILL THERE, no longer a layer: its pixels are its parent's
                 * again -- painted and presented above -- so it goes at once,
                 * with no exit to play over itself */
                if (r->n_doomed < SCENE_LAYERS) {
                    r->doomed[r->n_doomed].id = p->id; r->doomed[r->n_doomed].at = p->at; r->n_doomed++;
                } else r->host->go(r->host->ctx, p->id, 0, &p->at, 0, 0, 0, 1, 0);
                free(p->nodes); memset(p, 0, sizeof *p);
            } else if (r->promoted[i].id && !r->promoted[i].seen) promoted_leave(r, &r->promoted[i]);
            else if (r->promoted[i].waiting && !r->promoted[i].seen) memset(&r->promoted[i], 0, sizeof r->promoted[i]);
        }
    }
}
