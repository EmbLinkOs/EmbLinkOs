/* render.c -- see render.h. */
#include "render.h"
#include "glyphs.h"
#include <stdlib.h>
#include <string.h>

static int round_px(float v) { return (int)(v + .5f); }

bool term_render_fonts(struct term_render *r, struct font *const faces[4],
                       struct font *const *fallback, int nfallback, float size_px) {
    if (!faces[0] || size_px < 4) return false;
    struct font *f = faces[0];
    for (int i = 0; i < 4; i++) r->face[i] = faces[i] ? faces[i] : f;
    r->nfallback = nfallback < TERM_FALLBACK_MAX ? nfallback : TERM_FALLBACK_MAX;
    for (int i = 0; i < r->nfallback; i++) r->fallback[i] = fallback[i];
    r->size = size_px;
    float scale = size_px / (float)f->units_per_em;
    struct glyph_cache_entry *m = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, 'M', size_px);
    r->cell_w = m ? round_px(m->advance_px) : round_px(size_px * .6f);
    r->cell_h = round_px((f->ascent + f->descent + f->line_gap) * scale);
    r->baseline = round_px((f->ascent + f->line_gap / 2.0f) * scale);
    if (r->cell_w < 1) r->cell_w = 1;
    if (r->cell_h < 2) r->cell_h = 2;
    r->line = round_px(size_px / 14.0f);
    if (r->line < 1) r->line = 1;
    int below = r->cell_h - r->baseline;
    r->underline = r->baseline + (below / 3 > 1 ? below / 3 : 1);
    if (r->underline > r->cell_h - r->line) r->underline = r->cell_h - r->line;
    for (int i = 0; i < TERM_MASKS; i++) r->masks[i].w = 0;      /* the cell changed: redraw them */
    r->all = true;
    return true;
}

bool term_render_resize(struct term_render *r, int cols, int rows) {
    if (cols < 1 || rows < 1 || !r->cell_w) return false;
    int w = cols * r->cell_w, h = rows * r->cell_h;
    uint32_t *p = r->pixels;
    if (w != r->width || h != r->height || !p) {
        p = malloc((size_t)w * h * sizeof *p);
        if (!p) return false;
        free(r->pixels);
        r->pixels = p;
    }
    uint64_t *painted = realloc(r->painted, (size_t)rows * sizeof *painted);
    if (!painted) return false;
    r->painted = painted;
    r->cols = cols; r->rows = rows; r->width = w; r->height = h;
    r->all = true;
    return true;
}

void term_render_invalidate(struct term_render *r) { r->all = true; }

void term_render_free(struct term_render *r) {
    free(r->pixels); free(r->painted); free(r->next);
    for (int i = 0; i < TERM_MASKS; i++) free(r->masks[i].m);
    memset(r, 0, sizeof *r);
}

struct font *term_render_font_for(const struct term_render *r, uint32_t cp, int face) {
    if (font_has_glyph(r->face[face], cp)) return r->face[face];
    if (face && font_has_glyph(r->face[0], cp)) return r->face[0];
    for (int i = 0; i < r->nfallback; i++) if (r->fallback[i] && font_has_glyph(r->fallback[i], cp)) return r->fallback[i];
    return NULL;
}

/* --- pixels ---------------------------------------------------------------------- */

static void fill(struct term_render *r, int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->width) x1 = r->width;
    if (y1 > r->height) y1 = r->height;
    uint32_t px = 0xff000000u | rgb;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = r->pixels + (size_t)y * r->width;
        for (int x = x0; x < x1; x++) row[x] = px;
    }
}
/* Coverage blended in (approximately) linear light, as the toolkit's text is:
 * light text on a dark ground keeps its weight. Integers only: under
 * emulation a float is a library call, and this runs for every edge pixel. */
static uint16_t g_lin[256];          /* (v/255)^2, 0..65535 */
static uint8_t g_delin[4096];        /* back, from the top 12 bits */
static void gamma_tables(void) {
    if (g_lin[255]) return;
    for (int i = 0; i < 256; i++) g_lin[i] = (uint16_t)((i * i * 65535 + 255 * 255 / 2) / (255 * 255));
    for (int k = 0, v = 0; k < 4096; k++) {
        /* the smallest v whose square reaches k */
        while (v < 255 && (v * v * 4095) / (255 * 255) < k) v++;
        g_delin[k] = (uint8_t)v;
    }
}
static uint32_t blend(uint32_t dst, uint32_t fg, unsigned a) {
    if (a >= 255) return 0xff000000u | fg;
    unsigned inv = 255 - a;
    uint32_t out = 0xff000000u;
    for (int shift = 16; shift >= 0; shift -= 8) {
        uint32_t v = ((uint32_t)g_lin[(fg >> shift) & 255] * a + (uint32_t)g_lin[(dst >> shift) & 255] * inv) / 255;
        out |= (uint32_t)g_delin[v >> 4] << shift;
    }
    return out;
}
/* A coverage bitmap at (dx, dy), clipped to rows [cy0, cy1) of the target. */
static void blit(struct term_render *r, const uint8_t *cov, int stride, int gw, int gh,
                 int dx, int dy, uint32_t fg, int cy0, int cy1) {
    for (int y = 0; y < gh; y++) {
        int py = dy + y;
        if (py < cy0 || py >= cy1 || py < 0 || py >= r->height) continue;
        uint32_t *row = r->pixels + (size_t)py * r->width;
        const uint8_t *c = cov + (size_t)y * stride;
        for (int x = 0; x < gw; x++) {
            int px = dx + x;
            if (px < 0 || px >= r->width || !c[x]) continue;
            row[px] = blend(row[px], fg, c[x]);
        }
    }
}

/* --- one cell -------------------------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b) { return ((a & 0xfefefeu) >> 1) + ((b & 0xfefefeu) >> 1); }

struct look { uint32_t fg, bg, ul; uint16_t attrs; uint8_t underline; bool text; };
static struct look look_of(const struct vt *v, const struct vt_style *s, const struct term_frame *fr) {
    struct look k;
    uint32_t fgc = s->fg;
    if (fr->bold_is_bright && (s->attrs & VT_BOLD) && VT_COLOR_KIND(fgc) == 1 && (fgc & 0xff) < 8)
        fgc = VT_COLOR_INDEX((fgc & 0xff) + 8);
    k.fg = vt_rgb(v, fgc, true); k.bg = vt_rgb(v, s->bg, false);
    /* DECSCNM swaps the whole screen; a cell's own inverse swaps it back */
    if (((s->attrs & VT_INVERSE) != 0) != v->reverse_video) { uint32_t t = k.fg; k.fg = k.bg; k.bg = t; }
    if (s->attrs & VT_DIM) k.fg = mix(k.fg, k.bg);
    k.ul = s->ul == VT_COLOR_DEFAULT ? k.fg : vt_rgb(v, s->ul, true);
    k.attrs = s->attrs; k.underline = s->underline;
    k.text = !(s->attrs & VT_HIDDEN) && (fr->blink_on || !(s->attrs & (VT_BLINK | VT_RAPID)));
    return k;
}

static const uint8_t *drawn_glyph(struct term_render *r, uint32_t cp, int w, int h) {
    uint32_t slot = (cp * 2654435761u ^ (uint32_t)w * 40503u ^ (uint32_t)h * 97u) % TERM_MASKS;
    struct term_mask *m = &r->masks[slot];
    if (m->m && m->cp == cp && m->w == w && m->h == h) return m->m;
    uint8_t *g = realloc(m->m, (size_t)w * h);
    if (!g) return NULL;
    m->m = g; m->cp = cp; m->w = w; m->h = h;
    term_glyph_draw(cp, g, w, h, r->line);
    return g;
}

static void glyph(struct term_render *r, uint32_t cp, int face, int x, int y, int w, uint32_t fg, bool base) {
    if (cp == ' ' || cp < 0x20) return;
    if (term_glyph_is_procedural(cp)) {
        const uint8_t *m = drawn_glyph(r, cp, w, r->cell_h);
        if (m) blit(r, m, w, w, r->cell_h, x, y, fg, y, y + r->cell_h);
        return;
    }
    struct font *f = term_render_font_for(r, cp, face);
    if (!f) {
        if (!base) return;
        /* nothing draws it: a box, the width of the cell(s) */
        int t = r->line, x0 = x + 1, x1 = x + w - 1, y0 = y + 2, y1 = y + r->cell_h - 2;
        fill(r, x0, y0, x1, y0 + t, fg); fill(r, x0, y1 - t, x1, y1, fg);
        fill(r, x0, y0, x0 + t, y1, fg); fill(r, x1 - t, y0, x1, y1, fg);
        return;
    }
    struct glyph_atlas *atlas = font_global_atlas();
    struct glyph_cache_entry *e = glyph_cache_lookup_or_rasterize(atlas, f, cp, r->size);
    const uint8_t *cov = glyph_coverage(atlas, e);
    if (!cov) return;
    /* a face other than the terminal's: its own baseline, centred in the cells */
    int dx = x + e->bearing_x, dy = y + r->baseline - e->bearing_y;
    if (f != r->face[face] && f != r->face[0]) {
        int adv = round_px(e->advance_px);
        if (adv > 0 && adv < w) dx += (w - adv) / 2;
    }
    blit(r, cov, GLYPH_ATLAS_SIZE, e->atlas_w, e->atlas_h, dx, dy, fg, y, y + r->cell_h);
}

static void cell_text(struct term_render *r, const struct vt *v, const struct vt_cell *c, int face,
                      int x, int y, int w, uint32_t fg) {
    if (c->flags & VT_CELL_CLUSTER && c->cp < v->nclusters) {
        unsigned n = v->cluster_len[c->cp];
        for (unsigned i = 0; i < n; i++) glyph(r, v->clusters[c->cp][i], face, x, y, w, fg, i == 0);
    } else glyph(r, c->cp, face, x, y, w, fg, true);
}

static void decorations(struct term_render *r, const struct look *k, int x, int y, int w) {
    int t = r->line;
    if (k->underline) {
        int u = y + r->underline;
        switch (k->underline) {
        case VT_UL_DOUBLE: fill(r, x, u, x + w, u + t, k->ul); fill(r, x, u - 2 * t, x + w, u - t, k->ul); break;
        case VT_UL_CURLY:
            for (int i = 0; i < w; i++) {
                int phase = (x + i) % (4 * t + 4), up = phase < 2 * t + 2 ? phase : 4 * t + 4 - phase;
                int dy = up * 2 / (2 * t + 2);   /* 0..2 */
                fill(r, x + i, u - dy, x + i + 1, u - dy + t, k->ul);
            }
            break;
        case VT_UL_DOTTED: for (int i = 0; i < w; i++) if (((x + i) / (2 * t)) % 2 == 0) fill(r, x + i, u, x + i + 1, u + t, k->ul); break;
        case VT_UL_DASHED: for (int i = 0; i < w; i++) if ((x + i) % (6 * t) < 4 * t) fill(r, x + i, u, x + i + 1, u + t, k->ul); break;
        default: fill(r, x, u, x + w, u + t, k->ul); break;
        }
    }
    if (k->attrs & VT_STRIKE) { int s = y + r->baseline - (r->baseline * 3) / 10; fill(r, x, s, x + w, s + t, k->fg); }
    if (k->attrs & VT_OVERLINE) fill(r, x, y, x + w, y + t, k->fg);
}

/* --- rows ------------------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

/* What a row shows, as a number: its cells and their styles as the program
 * set them, the palette they resolve through (one hash a frame), the cursor
 * if it is on the row, the blink phase if anything on it blinks. */
static uint64_t row_signature(const struct vt *v, const struct vt_cell *cells, int cols,
                              const struct term_frame *fr, int y, uint64_t palette, bool *blinks) {
    uint64_t h = palette;
    for (int x = 0; x < cols; x++) {
        const struct vt_cell *c = &cells[x];
        const struct vt_style *s = vt_style_of(v, c);
        h = fnv(h, (uint64_t)c->cp << 16 | (uint64_t)c->width << 8 | c->flags);
        if (c->flags & VT_CELL_CLUSTER && c->cp < v->nclusters)
            for (unsigned i = 0; i < v->cluster_len[c->cp]; i++) h = fnv(h, v->clusters[c->cp][i]);
        if (c->style) {
            h = fnv(h, (uint64_t)s->fg << 32 | s->bg);
            h = fnv(h, (uint64_t)s->ul << 32 | (uint64_t)s->attrs << 8 | s->underline);
            if (s->attrs & (VT_BLINK | VT_RAPID)) { h = fnv(h, fr->blink_on); *blinks = true; }
        }
    }
    if (fr->cursor && fr->cursor_y == y)
        h = fnv(h, 0x100000000ull | (uint64_t)fr->cursor_x << 16 | (uint64_t)fr->cursor_shape << 8 | fr->cursor_hollow);
    return h;
}
/* Everything a row's colours depend on beyond its cells. */
static uint64_t palette_signature(const struct vt *v, const struct term_frame *fr) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, (uint64_t)v->reverse_video << 1 | fr->bold_is_bright);
    for (int i = 0; i < 256; i++) h = fnv(h, vt_rgb(v, VT_COLOR_INDEX(i), true));
    h = fnv(h, (uint64_t)vt_rgb(v, VT_COLOR_DEFAULT, true) << 32 | vt_rgb(v, VT_COLOR_DEFAULT, false));
    return fnv(h, vt_cursor_rgb(v));
}

static void paint_row(struct term_render *r, const struct vt *v, const struct vt_cell *cells,
                      const struct term_frame *fr, int row) {
    int y = row * r->cell_h, cols = r->cols < v->cols ? r->cols : v->cols;
    struct look k;
    /* the ground, then the glyphs over all of it (a glyph may lean into the
     * next cell), then lines, then the cursor */
    fill(r, 0, y, r->width, y + r->cell_h, vt_rgb(v, VT_COLOR_DEFAULT, v->reverse_video));
    for (int x = 0; x < cols; x++) {
        const struct vt_cell *c = &cells[x];
        if (!c->width) continue;
        k = look_of(v, vt_style_of(v, c), fr);
        fill(r, x * r->cell_w, y, (x + c->width) * r->cell_w, y + r->cell_h, k.bg);
    }
    bool cursor_here = fr->cursor && fr->cursor_y == row && fr->cursor_x >= 0 && fr->cursor_x < cols;
    int shape = fr->cursor_shape, cx = fr->cursor_x;
    bool block = cursor_here && !fr->cursor_hollow && shape <= 2;
    uint32_t cursor_rgb = vt_cursor_rgb(v);
    if (block) {
        int cw = cells[cx].width == 2 ? 2 : 1;
        fill(r, cx * r->cell_w, y, (cx + cw) * r->cell_w, y + r->cell_h, cursor_rgb);
    }
    for (int x = 0; x < cols; x++) {
        const struct vt_cell *c = &cells[x];
        if (!c->width) continue;
        const struct vt_style *s = vt_style_of(v, c);
        k = look_of(v, s, fr);
        if (!k.text) continue;
        int face = ((s->attrs & VT_BOLD) ? 1 : 0) | ((s->attrs & VT_ITALIC) ? 2 : 0);
        uint32_t fg = block && x == cx ? k.bg : k.fg;      /* the character under a block cursor */
        cell_text(r, v, c, face, x * r->cell_w, y, c->width * r->cell_w, fg);
    }
    for (int x = 0; x < cols; x++) {
        const struct vt_cell *c = &cells[x];
        if (!c->width) continue;
        k = look_of(v, vt_style_of(v, c), fr);
        if (k.text || (k.attrs & (VT_BLINK | VT_RAPID)) == 0) decorations(r, &k, x * r->cell_w, y, c->width * r->cell_w);
    }
    if (cursor_here && !block) {
        int x0 = cx * r->cell_w, w = (cells[cx].width == 2 ? 2 : 1) * r->cell_w, t = r->line;
        if (fr->cursor_hollow && shape <= 2) {
            fill(r, x0, y, x0 + w, y + t, cursor_rgb); fill(r, x0, y + r->cell_h - t, x0 + w, y + r->cell_h, cursor_rgb);
            fill(r, x0, y, x0 + t, y + r->cell_h, cursor_rgb); fill(r, x0 + w - t, y, x0 + w, y + r->cell_h, cursor_rgb);
        } else if (shape == 3 || shape == 4) fill(r, x0, y + r->cell_h - 2 * t, x0 + w, y + r->cell_h, cursor_rgb);
        else fill(r, x0, y, x0 + 2 * t, y + r->cell_h, cursor_rgb);
    }
}

bool term_render_frame(struct term_render *r, const struct vt *v, const struct term_frame *fr) {
    if (!r->pixels || !r->painted) return false;
    gamma_tables();
    r->dmg_y0 = r->height; r->dmg_y1 = 0;
    r->blink_text = false;
    r->rows_painted = r->rows_moved = 0;
    int rows = r->rows < v->rows ? r->rows : v->rows;
    uint64_t *sig = realloc(r->next, (size_t)r->rows * sizeof *sig);
    if (!sig) return false;
    r->next = sig;
    uint64_t palette = palette_signature(v, fr);
    uint32_t ground = vt_rgb(v, VT_COLOR_DEFAULT, v->reverse_video);
    for (int row = 0; row < r->rows; row++) {
        const struct vt_cell *cells = row < rows ? vt_view_row(v, row, fr->back) : NULL;
        sig[row] = cells ? row_signature(v, cells, v->cols, fr, row, palette, &r->blink_text)
                         : 0x9e3779b97f4a7c15ull ^ ground;
    }
    /* A scroll: last frame's rows, moved up. Move their pixels too, and paint
     * only what is new -- output scrolling by is one row's work a frame, not
     * the window's. The shift taken is the one that leaves most rows right. */
    if (!r->all && r->rows > 2) {
        int still = 0, best_k = 0, best = 0;
        for (int y = 0; y < r->rows; y++) still += sig[y] == r->painted[y];
        for (int k = 1; k < r->rows && best < r->rows - k; k++) {
            int n = 0;
            for (int y = 0; y + k < r->rows; y++) n += sig[y] == r->painted[y + k];
            if (n > best) { best = n; best_k = k; }
        }
        if (best_k && best > still && best >= (r->rows - best_k) / 2) {
            size_t row_px = (size_t)r->cell_h * r->width;
            memmove(r->pixels, r->pixels + best_k * row_px, (size_t)(r->rows - best_k) * row_px * sizeof *r->pixels);
            memmove(r->painted, r->painted + best_k, (size_t)(r->rows - best_k) * sizeof *r->painted);
            for (int y = r->rows - best_k; y < r->rows; y++) r->painted[y] = ~sig[y];
            r->dmg_y0 = 0; r->dmg_y1 = (r->rows - best_k) * r->cell_h;
            r->rows_moved = r->rows - best_k;
        }
    }
    for (int row = 0; row < r->rows; row++) {
        if (!r->all && r->painted[row] == sig[row]) continue;
        const struct vt_cell *cells = row < rows ? vt_view_row(v, row, fr->back) : NULL;
        if (cells) paint_row(r, v, cells, fr, row);
        else fill(r, 0, row * r->cell_h, r->width, (row + 1) * r->cell_h, ground);
        r->painted[row] = sig[row];
        r->rows_painted++;
        if (row * r->cell_h < r->dmg_y0) r->dmg_y0 = row * r->cell_h;
        if ((row + 1) * r->cell_h > r->dmg_y1) r->dmg_y1 = (row + 1) * r->cell_h;
    }
    r->all = false;
    return r->dmg_y1 > r->dmg_y0;
}
