#ifndef __EMBLINK_UI_TESTFONT_H__
#define __EMBLINK_UI_TESTFONT_H__

/* ui/testfont.h -- a synthetic TTF for host tests, with KNOWN advances.
 *
 * WHY A HOST UI TEST NEEDS ONE AT ALL. Without a font loaded, every text
 * measurement in the toolkit returns zero -- and a zero-width string does not
 * fail loudly, it fails QUIETLY and plausibly. A click-to-place test written
 * against it will happily report that the caret went where it was aimed,
 * because every position measures the same: nowhere.
 *
 * That is not hypothetical. The single-line field's drag-selection test passed
 * for a while against a fontless kit, and it passed for the wrong reason: with
 * all widths zero, a drag from inside the field to far outside it still
 * produced caret 0 and caret len, which is the right ANSWER by accident of
 * geometry, having measured nothing.
 *
 * WHAT THIS IS: every glyph blank, every advance 500/1000 em -- so at text
 * size S each character is exactly S/2 pixels wide, and a test can say where
 * the caret should land instead of asking. unitsPerEm 1000, ascent 800,
 * descent -200, a cmap covering U+0020..U+007E (glyph = codepoint), and one
 * hmtx entry, which by the TTF rules gives every later glyph the same advance
 * -- so characters outside the cmap, é among them, are 500 too.
 *
 * Header-only and self-contained: each test gets its own copy of the bytes,
 * which is a few KB and saves a link-order dependency between test binaries. */

#include <stdint.h>

struct testfont { uint8_t data[8192]; uint32_t len; };

static inline void tf_w8 (struct testfont *f, uint8_t v)  { f->data[f->len++] = v; }
static inline void tf_w16(struct testfont *f, uint16_t v) { tf_w8(f, (uint8_t)(v >> 8)); tf_w8(f, (uint8_t)v); }
static inline void tf_w32(struct testfont *f, uint32_t v) { tf_w16(f, (uint16_t)(v >> 16)); tf_w16(f, (uint16_t)v); }
static inline void tf_wi16(struct testfont *f, int16_t v) { tf_w16(f, (uint16_t)v); }
static inline void tf_w16_at(struct testfont *f, uint32_t o, uint16_t v) { f->data[o] = (uint8_t)(v >> 8); f->data[o+1] = (uint8_t)v; }
static inline void tf_w32_at(struct testfont *f, uint32_t o, uint32_t v) { tf_w16_at(f, o, (uint16_t)(v >> 16)); tf_w16_at(f, o+2, (uint16_t)v); }

/* Fills `f` and returns its length; pass f->data and the return to font_load. */
static inline uint32_t testfont_build(struct testfont *f) {
    f->len = 0;
    const int nt = 7;
    tf_w32(f, 0x00010000); tf_w16(f, (uint16_t)nt); tf_w16(f, 0); tf_w16(f, 0); tf_w16(f, 0);
    uint32_t dir = f->len;
    for (int i = 0; i < nt; i++) { tf_w32(f,0); tf_w32(f,0); tf_w32(f,0); tf_w32(f,0); }
    struct { const char *tag; uint32_t off, len; } rec[7];
    int ri = 0;
    #define TF_BEG(T) do { rec[ri].tag = (T); rec[ri].off = f->len; } while (0)
    #define TF_END()  do { rec[ri].len = f->len - rec[ri].off; while (f->len & 3) tf_w8(f,0); ri++; } while (0)

    TF_BEG("head"); { uint32_t s = f->len; for (int i=0;i<54;i++) tf_w8(f,0);
        tf_w32_at(f, s+0, 0x00010000); tf_w16_at(f, s+18, 1000); tf_w16_at(f, s+50, 1); } TF_END();
    TF_BEG("maxp"); tf_w32(f, 0x00010000); tf_w16(f, 128); TF_END();
    TF_BEG("hhea"); { uint32_t s = f->len; for (int i=0;i<36;i++) tf_w8(f,0);
        tf_w16_at(f, s+4, 800); tf_w16_at(f, s+6, (uint16_t)(int16_t)-200);
        tf_w16_at(f, s+8, 0); tf_w16_at(f, s+34, 1); } TF_END();
    TF_BEG("hmtx"); tf_w16(f, 500); tf_w16(f, 0); TF_END();      /* one metric: advance 500 */
    TF_BEG("cmap"); { uint32_t cs = f->len;
        tf_w16(f,0); tf_w16(f,1); tf_w16(f,3); tf_w16(f,1); tf_w32(f,12);
        tf_w16(f,4); uint32_t lo = f->len; tf_w16(f,0); tf_w16(f,0);
        tf_w16(f,4); tf_w16(f,4); tf_w16(f,1); tf_w16(f,0);
        tf_w16(f,0x007e); tf_w16(f,0xFFFF);        /* endCode   */
        tf_w16(f,0);                                /* pad       */
        tf_w16(f,0x0020); tf_w16(f,0xFFFF);        /* startCode */
        tf_wi16(f,0); tf_w16(f,1);                  /* idDelta: glyph = codepoint */
        tf_w16(f,0); tf_w16(f,0);                   /* idRangeOffset */
        tf_w16_at(f, lo, (uint16_t)(f->len - cs - 12)); } TF_END();
    TF_BEG("glyf"); TF_END();                                    /* empty: all blank */
    TF_BEG("loca"); for (int i=0;i<129;i++) tf_w32(f,0); TF_END();

    for (int i = 0; i < nt; i++) {
        uint32_t r = dir + (uint32_t)i * 16;
        f->data[r]   = (uint8_t)rec[i].tag[0]; f->data[r+1] = (uint8_t)rec[i].tag[1];
        f->data[r+2] = (uint8_t)rec[i].tag[2]; f->data[r+3] = (uint8_t)rec[i].tag[3];
        tf_w32_at(f, r+4, 0); tf_w32_at(f, r+8, rec[i].off); tf_w32_at(f, r+12, rec[i].len);
    }
    #undef TF_BEG
    #undef TF_END
    return f->len;
}

/* Pixels per character at text size `sz`, which is what a test computes its
 * expected caret positions from. */
static inline float testfont_advance(float sz) { return sz * 0.5f; }

#endif /* __EMBLINK_UI_TESTFONT_H__ */
