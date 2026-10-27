#ifndef __EMBLINK_UI_FONT_H__
#define __EMBLINK_UI_FONT_H__

/* ui/backend/font.h -- EmbLink UI Piece 4b: the font rasterizer.
 *
 * Fills in Piece 4a's declared-but-NULL draw_text vtable entry. Pure userland
 * C, no kernel involvement. Direction (a) from the 4a fork: a full runtime
 * TrueType (glyf-outline) parser + rasterizer, entirely inside EmbLink, made
 * affordable by a glyph atlas so each (font, codepoint, size) is rasterized
 * exactly once ever.
 *
 * Scope (Section 0, decided not asked): TrueType glyf only (no CFF/OpenType);
 * no hinting; supersample+box-downsample AA; cmap formats 4 (the BMP) and 12
 * (every plane: icons in the supplementary private use areas, emoji); simple
 * offset/scale composites; no kerning; no shaping (LTR, 1 codepoint -> 1
 * glyph). Each omission is a named gap (Section 8), not an oversight. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "backend.h"    /* struct render_target, struct color */

/* ------------------------------------------------------------------------- */
/* Section 1: the parsed font                                                */
/* ------------------------------------------------------------------------- */

struct font {
    const uint8_t *data;        /* caller-owned, resident for the font's lifetime */
    size_t          data_len;

    uint16_t units_per_em;       /* 'head' -- outline coord scale; px = coord * size_px/units_per_em */
    int16_t  index_to_loc_fmt;   /* 'head' -- 0: loca is uint16*2, 1: uint32 */
    uint16_t num_glyphs;         /* 'maxp' */

    uint32_t glyf_offset, glyf_len;
    uint32_t loca_offset, loca_len;
    uint32_t cmap_subtable_offset;  /* the chosen subtable itself */
    uint16_t cmap_format;           /* 12 (all planes) when the font has one, else 4 */
    uint32_t hmtx_offset;
    uint16_t num_h_metrics;         /* 'hhea' */
    /* vertical metrics ('hhea', same table as num_h_metrics). ascent/line_gap
     * as-read; descent is stored POSITIVE (negated from the file's negative
     * descender) so line_height = (ascent + descent + line_gap) * scale --
     * Piece 5's layout line-height formula. */
    int16_t  ascent, descent, line_gap;
    bool     used;
    /* THE FALLBACK: a face (handle, 0: none) for the codepoints this one does
     * not have -- the UI face is chosen for how it reads, the fallback for how
     * much it covers, and text should not turn to boxes at the first symbol
     * the UI face lacks. */
    uint32_t fallback;
    /* KERNING, from GPOS (font_kern_px): the pair-adjustment lookups the
     * 'kern' feature names, and the ASCII pairs worked out once, the first
     * time any is asked for -- text is mostly ASCII and measured every frame. */
    uint32_t gpos_offset, gpos_len;
    uint16_t kern_lookups[16];
    uint8_t  n_kern_lookups;
    int16_t *kern_ascii;          /* 95 x 95 font units, ' '..'~', or NULL */
    /* DYNAMIC TRACKING (font_pair_px): letter-spacing that depends on the size,
     * as the face's designer publishes it -- Inter's dynamic metrics, read from
     * the family name. Large type closes up, small type keeps its air. */
    bool     dyn_tracking;
    float    trk_size, trk_px;      /* the last size asked for, and its tracking */
};

/* Parse the sfnt table directory once; -> a handle (index into a table of
 * FONT_MAX, 1-based; 0 == failure). No checksum validation -- later bounds checks
 * fail the specific lookup, not the load. Nothing is rasterized here. */
#define FONT_MAX 16
uint32_t font_load(const uint8_t *ttf_data, size_t len);
struct font *font_for_handle(uint32_t handle);
/* How much closer (negative) or further apart the pair `left` `right` sits
 * than their advances say, in pixels at `size_px` -- the face's own kerning
 * (GPOS pair adjustment). 0 for a pair the face does not kern, or whose
 * glyphs come from different faces. Every walk over text adds it, so a caret
 * stands where the glyphs are. */
float font_kern_px(struct font *f, uint32_t left, uint32_t right, float size_px);
/* What every walk over text adds between `left` and `right`: their kerning and
 * the face's tracking at that size (dyn_tracking). 0 before the first glyph. */
float font_pair_px(struct font *f, uint32_t left, uint32_t right, float size_px);
/* Codepoints `font` lacks come from `fallback` (0 clears). */
void font_set_fallback(uint32_t font, uint32_t fallback);
/* The face that draws `codepoint`: `f` when it has it, else its fallback when
 * that does, else `f` (its .notdef box). */
struct font *font_for_codepoint(struct font *f, uint32_t codepoint);

/* ------------------------------------------------------------------------- */
/* Section 2: cmap (formats 12 and 4)                                        */
/* ------------------------------------------------------------------------- */

/* -> glyph index, or 0 (.notdef) if `codepoint` isn't in the font. */
uint32_t font_codepoint_to_glyph(struct font *f, uint32_t codepoint);
/* Does the font draw `codepoint` at all? A caller with a fallback chain asks
 * before it draws, rather than drawing a .notdef box. */
bool font_has_glyph(struct font *f, uint32_t codepoint);

/* ------------------------------------------------------------------------- */
/* Section 3: glyf/loca outlines                                             */
/* ------------------------------------------------------------------------- */

struct glyph_point   { float x, y; bool on_curve; };
struct glyph_contour { struct glyph_point *points; uint32_t n_points; };

struct glyph_outline {
    struct glyph_contour *contours;
    uint32_t               n_contours;
    int16_t  x_min, y_min, x_max, y_max;   /* font units */
    uint16_t advance_width;                 /* font units, from hmtx */
    int16_t  left_side_bearing;
};

/* Extract glyph `glyph_index`'s outline (simple or composite). Allocates
 * contours/points; free with font_free_outline. An empty range (space) is a
 * valid outline with n_contours == 0. */
bool font_get_glyph_outline(struct font *f, uint32_t glyph_index, struct glyph_outline *out);
void font_free_outline(struct glyph_outline *o);

/* ------------------------------------------------------------------------- */
/* Section 4: rasterization (outline -> 8-bit coverage bitmap)               */
/* ------------------------------------------------------------------------- */

/* out_coverage: freshly malloc'd out_w*out_h bytes (single 8-bit alpha). The
 * caller copies it into the atlas and frees it. bearing/advance in PIXELS. */
bool font_rasterize_glyph(struct font *f, uint32_t glyph_index, float size_px,
                          uint8_t **out_coverage, int *out_w, int *out_h,
                          int *out_bearing_x, int *out_bearing_y, float *out_advance_px);
/* ...with the outline moved right by `dx` (0 <= dx < 1) of a pixel first:
 * a glyph for a pen that is not on a whole pixel. */
bool font_rasterize_glyph_at(struct font *f, uint32_t glyph_index, float size_px, float dx,
                             uint8_t **out_coverage, int *out_w, int *out_h,
                             int *out_bearing_x, int *out_bearing_y, float *out_advance_px);
/* SUBPIXEL POSITIONING: a glyph is rasterised at one of FONT_PHASES offsets
 * of a pixel, and drawn at the one nearest its pen. A pen rounded to whole
 * pixels makes the space between letters uneven -- "rn" one pixel tighter
 * than "nn" -- by up to half a pixel each, which at text sizes is a fifth of
 * a stem. Quarters are the phases desktops settle on. */
#define FONT_PHASES 4

/* Debug counter: how many times font_rasterize_glyph actually ran (T4 asserts
 * the cache stops this incrementing on a repeat lookup). */
uint32_t font_debug_rasterize_count(void);

/* Debug: flatten contour 0 of glyph `gi` to a pixel-space polyline (T2 checks
 * the curve actually bulges off its endpoint chord). Returns the point count;
 * copies up to `max` into xs/ys. */
int font_debug_flatten_contour0(struct font *f, uint32_t gi, float scale,
                                float *xs, float *ys, int max);

/* ------------------------------------------------------------------------- */
/* Section 5: glyph atlas (cache)                                            */
/* ------------------------------------------------------------------------- */

/* The atlas is up to GLYPH_ATLAS_PAGES pages of GLYPH_ATLAS_SIZE^2 coverage
 * bytes, allocated as they are needed; a process that draws little text holds
 * one. Lookups go through a hash of (font, codepoint, size). When every page
 * is full, the page used longest ago is emptied and its glyphs forgotten --
 * a terminal full of icons and three faces does not wipe the whole cache and
 * rasterize every glyph again on the next frame. */
#define GLYPH_ATLAS_SIZE  1024
#define GLYPH_ATLAS_PAGES 4
#define GLYPH_CACHE_MAX   8192   /* distinct (font,codepoint,size) entries */
#define GLYPH_HASH_SIZE   16384  /* power of two */
#define GLYPH_NO_PAGE     0xff   /* a glyph with no pixels (space) */

struct glyph_cache_entry {
    uint32_t font_handle, codepoint;
    uint16_t size_px_x4;          /* size_px * 4 rounded -> collapses float noise */
    uint8_t  phase;               /* which FONT_PHASES offset it was rasterised at */
    uint16_t atlas_x, atlas_y, atlas_w, atlas_h;
    int16_t  bearing_x, bearing_y;
    float    advance_px;
    bool     used;
    bool     missing;             /* the font has no glyph: this is its .notdef */
    uint8_t  page;                /* which atlas page, or GLYPH_NO_PAGE */
    uint32_t next;                /* hash chain: entry index + 1, 0 ends it */
};

struct glyph_page {
    uint8_t *coverage;            /* GLYPH_ATLAS_SIZE * GLYPH_ATLAS_SIZE, or NULL until needed */
    uint32_t shelf_x, shelf_y, shelf_row_h;   /* shelf/row packer cursor */
    uint64_t last_use;
};

struct glyph_atlas {
    struct glyph_page pages[GLYPH_ATLAS_PAGES];
    uint32_t current;             /* the page being filled */
    struct glyph_cache_entry entries[GLYPH_CACHE_MAX];
    uint32_t n_entries;           /* high-water mark */
    uint32_t free_head;           /* evicted entries: index + 1, 0 = none */
    uint32_t hash[GLYPH_HASH_SIZE];   /* entry index + 1, 0 = empty */
    uint64_t clock;
};

/* Lookup by (font_handle, codepoint, size_px_x4); on miss, rasterize + pack.
 * The entry is valid until the next lookup (that one may evict it). */
struct glyph_cache_entry *glyph_cache_lookup_or_rasterize(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px);
/* ...at a phase (0 .. FONT_PHASES - 1): the glyph moved right phase/FONT_PHASES
 * of a pixel. Advances are the same at every phase. */
struct glyph_cache_entry *glyph_cache_lookup_phase(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px, uint32_t phase);
/* The entry's coverage: atlas_h rows of atlas_w bytes, GLYPH_ATLAS_SIZE apart.
 * NULL for a glyph with no pixels. */
const uint8_t *glyph_coverage(const struct glyph_atlas *atlas, const struct glyph_cache_entry *e);

/* ------------------------------------------------------------------------- */
/* Section 6: draw_text -- installs into the CPU backend's vtable slot        */
/* ------------------------------------------------------------------------- */

/* Point cpu_backend_get()->draw_text at the real implementation. Call once at
 * startup (the vtable ships with draw_text == NULL until then). */
void font_install_backend(void);
/* The renderer's own UTF-8 decoder. Anything that MEASURES text must step the
 * string with this, or it will measure a different string than gets drawn. */
int  font_utf8_decode(const char *s, uint32_t *cp);

typedef void (*font_text_visitor)(void *context,uint32_t codepoint,float x,float y);
/* Shared UTF-8 word/character wrapping. A zero width disables automatic wrapping. */
int font_layout_text(struct font *font,const char *text,float size,float width,
                     font_text_visitor visit,void *context);

/* Exposed for tests: the shared process-wide atlas draw_text uses. */
struct glyph_atlas *font_global_atlas(void);

#endif /* __EMBLINK_UI_FONT_H__ */
