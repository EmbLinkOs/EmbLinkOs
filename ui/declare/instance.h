#ifndef __EMBLINK_UI_INSTANCE_H__
#define __EMBLINK_UI_INSTANCE_H__

/* ui/declare/instance.h -- EmbLink UI Piece 7: the retained instance tree.
 *
 * A "Button" or "VStack" is not a distinct storage kind -- both are an
 * INSTANCE_BOX with different property presets. Same paged-arena /
 * {index,generation} ABA-guard discipline as Pieces 3/5/6. Each instance pairs
 * with a Piece-3 scene node AND a Piece-5 layout node it drives. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "scene.h"    /* node_handle, struct paint, struct color */
#include "layout.h"   /* layout_handle, struct layout_size */
#include "scope.h"    /* scope_handle (Piece 6) */

struct instance_handle { uint32_t index, generation; };
static const struct instance_handle INSTANCE_HANDLE_NULL = { 0, 0 };
static inline bool instance_handle_is_null(struct instance_handle h) { return h.index == 0; }

enum instance_kind { INSTANCE_TEXT, INSTANCE_BOX, INSTANCE_IMAGE, INSTANCE_COMPONENT };

/* Shadow of the last-declared property values, for no-op-skip diffing (§5):
 * a mutation into Piece 3/5 only fires when the declared value actually
 * differs from what's stored here. */
struct ui_shadow {
    bool has_text;   char text[256]; uint32_t font_handle; float text_size; struct color text_color;
    bool has_paint;  struct paint paint;
    bool has_corner; float corner_radius;
    bool has_pad;    float pt, pr, pb, pl;
    bool has_spacing; float spacing;
    bool has_size;   struct layout_size sw, shh;
    bool has_clip;   bool clip;
};

struct instance {
    struct instance_handle self, parent, first_child, next_sibling;

    enum instance_kind kind;
    /* WHICH WAY A CONTAINER STACKS, and part of its identity.
     *
     * Every container is INSTANCE_BOX, so before this a VStack and an HStack in
     * the same slot MATCHED each other and the second reused the first's
     * instance -- inheriting every box property the new one does not restate,
     * because the shadow below only fires a mutation when a declared value
     * DIFFERS. Files' empty-folder placeholder (a VStack with 32/24 padding)
     * was reused as the first row of the grid (an HStack that sets none), and
     * the listing drew 32 px down and 24 px right of where it belonged.
     *
     * 0 = a plain box or a leaf, 1 = column, 2 = row. Comparing it only ever
     * makes matching STRICTER: the worst case is one extra destroy-and-create
     * where two different containers really do trade places. */
    uint8_t  box_axis;
    bool     has_explicit_key;
    uint64_t explicit_key;

    struct node_handle   scene_node;    /* paired Piece-3 node (every kind has one) */
    struct layout_handle layout_node;   /* paired Piece-5 node */

    /* component-specific */
    struct scope_handle scope;
    void  (*component_fn)(void *props);
    void   *props_copy;                  /* OWNED heap copy (§4) */
    size_t  props_size;

    bool clicked_this_frame;             /* one-frame pulse, set by hit dispatch (§7) */
    bool visited_this_run;               /* reconciliation bookkeeping */

    struct ui_shadow shadow;
    bool used;
};

struct instance *instance_resolve(struct instance_handle h);   /* NULL if stale */

#endif /* __EMBLINK_UI_INSTANCE_H__ */
