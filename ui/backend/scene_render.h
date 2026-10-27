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

#endif /* __EMBLINK_UI_SCENE_RENDER_H__ */
