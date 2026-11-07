#ifndef __EMBLINK_UI_SCENE_RENDER_H__
#define __EMBLINK_UI_SCENE_RENDER_H__

/* ui/backend/scene_render.h -- EmbLink UI Piece 4a: the traversal DRIVER.
 *
 * Bridges Piece 3's scene tree to the render_backend vtable: maps each node
 * kind to its draw call, does grouped-opacity offscreen compositing (Section
 * 5), and runs the dirty-rect algorithm (Section 6 -- the crux) so per-pixel
 * work stays bounded to what actually changed while the tree walk itself
 * covers everything. This is the same machinery a single client uses for its
 * own window AND the compositor uses to composite every window (Section 7):
 * no separate multi-window path, just this tree, one level up. */

#include "backend.h"
#include "scene.h"

struct rect_cache { float x, y, w, h; uint32_t generation; int valid; };

/* A GROUP'S KEPT PIXELS (Section 5, cached). An opacity or scale group is
 * painted whole into a buffer of its own and then composited once; while
 * nothing inside it changes -- only how it is composited: its opacity, its
 * scale, where it stands by whole pixels -- the buffer is still right, and
 * the frame composites it again instead of painting the subtree. A menu
 * fading in is then one blit a frame, not its whole tree. */
#define SCENE_GROUP_CACHE 16
struct group_cache {
    uint32_t  index, generation;   /* the group's root node */
    uint32_t *px; uint32_t w, h;   /* premultiplied, w x h, as painted */
    uint32_t  cap;                 /* pixels the buffer holds */
    float     fx, fy;              /* where it stood, past the whole pixel */
    uint32_t  sig;                 /* the subtree's shape when painted */
    int       reduced;             /* the effects tier it was painted at */
    int       used;                /* met this frame */
};

/* LAYERS: who keeps promoted subtrees (scene_node.promoted) for the renderer
 * -- the application runtime, which hands them to the compositor
 * (user/lib/embk_layer.h). The renderer paints a promoted subtree once into a
 * layer, and says where it is to be and how it gets there; the host does the
 * rest. Coordinates are the window's. */
struct scene_layer_state {
    int32_t  x, y;                 /* the layer's top-left at full size */
    uint8_t  opacity;              /* 0..255 */
    uint16_t scale_milli, anchor_x_milli, anchor_y_milli;
};
struct scene_layer_host {
    void *ctx;
    /* a layer w x h: its id (> 0), its pixels -- premultiplied, w a row,
     * transparent -- in *px; 0 when there is none to be had */
    uint32_t (*create)(void *ctx, uint32_t w, uint32_t h, uint32_t **px);
    /* its pixels in (x, y, w, h) changed */
    void (*present)(void *ctx, uint32_t id, int x, int y, int w, int h);
    /* go to `to` on the spring (response ms, damping permille; 0: jump), from
     * `from` if given (jumped to first); then gone when `destroy`. With
     * `at_present` it takes effect at the layer's next present. */
    void (*go)(void *ctx, uint32_t id, const struct scene_layer_state *from, const struct scene_layer_state *to,
               uint16_t response_ms, uint16_t damping_milli, int destroy, int at_present);
    /* how it looks beyond its pixels, said once when it is made (may be NULL):
     * its corner radius, and glass -- the compositor blurs what is really
     * behind it, other windows too, under its pixels; the renderer paints the
     * glass's tint alone */
    void (*look)(void *ctx, uint32_t id, uint8_t radius, int glass, int x, int y, int w, int h, int fill);
    /* fill: the layer is one colour -- its single pixel -- standing w x h */
};
#define SCENE_LAYERS 8
struct scene_promoted {
    uint32_t  index, generation;   /* the promoted node */
    uint32_t  id;                  /* the host's layer */
    uint32_t *px; uint32_t w, h;
    struct scene_layer_state at;   /* where it was last told to rest */
    struct scene_layer_motion disappear;
    float     nx, ny, nw, nh;      /* the node's own box: where its anchors are */
    float     fx, fy;              /* its place past the whole pixel, as painted */
    uint32_t  sig;
    int       reduced, seen;
    int       waiting;             /* seen, not yet settled: no layer yet (frames waited) */
    /* where each node of it painted last, layer pixels, in walk order -- so a
     * change inside repaints only what it changed (a hover, a caret) */
    struct scene_promoted_node { float x, y, w, h; } *nodes;
    uint32_t  n_nodes, cap_nodes;
    int       fill;                /* one colour: a 1 x 1 layer standing w x h */
};

struct scene_renderer {
    struct render_backend *be;
    struct rect_cache     *cache;      /* per-node world rect from last frame */
    uint32_t               cache_cap;
    struct clip_rect       dirty[16];  /* this frame's accumulator (Section 6) */
    int                    n_dirty;
    int                    full;       /* cap exceeded -> one full-screen rect */
    /* Scroll-blit result (Section 6b): when a frame was resolved as a pure
     * scroll, only the exposed STRIP was repainted -- but the whole blitted
     * region's pixels changed, so a consumer that copies/presents by dirty
     * rect must cover THIS rect too. Valid when has_scroll_present. */
    float                  sp_x, sp_y, sp_w, sp_h;
    int                    has_scroll_present;
    /* Inside an opacity group: the target the outermost group is composited
     * onto, and its offset -- where glass in the group finds its backdrop. */
    struct render_target  *backdrop;
    float                  bd_ox, bd_oy;
    /* Painting a group's subtree into its own buffer: cull nothing. The frame's
     * dirty rects say what is to change ON SCREEN; the buffer is composited
     * whole (and, scaled, not where its children's own rects say), so every
     * child belongs in it. */
    int                    no_cull;
    struct group_cache     groups[SCENE_GROUP_CACHE];
    /* frames whose groups were composited from kept pixels / painted anew */
    uint32_t               group_hits, group_paints;
    /* the layer host, and the promoted subtrees it holds for us */
    const struct scene_layer_host *host;
    struct scene_promoted  promoted[SCENE_LAYERS];
    uint32_t               layer_paints;   /* promoted subtrees painted whole (tests) */
    uint32_t               layer_partials; /* ...and painted only where they changed */
    int                    wants_frame;    /* a promoted subtree is settling: build the next */
    const struct scene_node *glass_root;   /* painting it into its layer: its blur is the compositor's */
};

void scene_render_init(struct scene_renderer *r, struct render_backend *be);
void scene_render_destroy(struct scene_renderer *r);

/* Render one frame of `root`'s tree into `target`, repainting only dirty
 * regions (everything on the first frame, since nothing is cached yet). */
void scene_render_frame(struct scene_renderer *r, struct scene_arena *a,
                        struct node_handle root, struct render_target *target);

/* Forget every cached rect, so the next frame repaints the whole tree.
 *
 * For a caller that knows the surface changed for a reason the cache cannot
 * see. The cache is keyed by NODE INDEX, and indices are reused: a subtree that
 * is destroyed and later rebuilt identically -- a launcher opening a second
 * time with the same apps in the same places -- lands on the same indices
 * holding still-valid entries with the same geometry, so every node compares
 * equal and not one pixel is painted. The declarative pass runs, the app
 * believes it drew, and the screen keeps the frame from before. */
void scene_render_invalidate(struct scene_renderer *r);
/* THE REDUCED EFFECTS TIER (the compositor's, embk_comp_stats().effects): glass
 * draws no blur, only its own tint made whole. Per process; 1 if it changed --
 * the caller then invalidates its renderer so every glass node is drawn anew. */
int  scene_render_set_reduced(int on);
/* Hand promoted subtrees to `host` (NULL: paint them in the window, as any
 * other subtree). Layers it holds are let go when it is changed. */
void scene_render_set_layer_host(struct scene_renderer *r, const struct scene_layer_host *host);

#endif /* __EMBLINK_UI_SCENE_RENDER_H__ */
