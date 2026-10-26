/* glyphs.c -- see glyphs.h. */
#include "glyphs.h"
#include <string.h>

/* U+2500-257F as four arm weights, up right down left: 0 none, 1 light,
 * 2 heavy, 3 double. The dashed lines, arcs and diagonals among them are
 * drawn by kind (kind_of). */
static const char box[128][5] = {
    "0101","0202","1010","2020","0101","0202","1010","2020",   /* 2500 */
    "0101","0202","1010","2020","0110","0210","0120","0220",   /* 2508 */
    "0011","0012","0021","0022","1100","1200","2100","2200",   /* 2510 */
    "1001","1002","2001","2002","1110","1210","2110","1120",   /* 2518 */
    "2120","2210","1220","2220","1011","1012","2011","1021",   /* 2520 */
    "2021","2012","1022","2022","0111","0112","0211","0212",   /* 2528 */
    "0121","0122","0221","0222","1101","1102","1201","1202",   /* 2530 */
    "2101","2102","2201","2202","1111","1112","1211","1212",   /* 2538 */
    "2111","1121","2121","2112","2211","1122","1221","2212",   /* 2540 */
    "1222","2122","2221","2222","0101","0202","1010","2020",   /* 2548 */
    "0303","3030","0310","0130","0330","0013","0031","0033",   /* 2550 */
    "1300","3100","3300","1003","3001","3003","1310","3130",   /* 2558 */
    "3330","1013","3031","3033","0313","0131","0333","1303",   /* 2560 */
    "3101","3303","1313","3131","3333","0110","0011","1001",   /* 2568 */
    "1100","0000","0000","0000","0001","1000","0100","0010",   /* 2570 */
    "0002","2000","0200","0020","0201","1020","0102","2010",   /* 2578 */
};
enum { PLAIN, DASH2, DASH3, DASH4, ARC, DIAGONAL };
static int kind_of(uint32_t cp) {
    if (cp >= 0x2504 && cp <= 0x2507) return DASH3;
    if (cp >= 0x2508 && cp <= 0x250b) return DASH4;
    if (cp >= 0x254c && cp <= 0x254f) return DASH2;
    if (cp >= 0x256d && cp <= 0x2570) return ARC;
    if (cp >= 0x2571 && cp <= 0x2573) return DIAGONAL;
    return PLAIN;
}

bool term_glyph_is_procedural(uint32_t cp) {
    return (cp >= 0x2500 && cp <= 0x259f) || (cp >= 0x2800 && cp <= 0x28ff) || (cp >= 0xe0b0 && cp <= 0xe0bf);
}

/* --- painting into the mask ---------------------------------------------------- */

struct canvas { uint8_t *m; int w, h; };
static void fill(const struct canvas *c, int x0, int y0, int x1, int y1, uint8_t v) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) if (c->m[y * c->w + x] < v) c->m[y * c->w + x] = v;
}
/* A shape by its inside test, 4x4 samples a pixel: smooth edges. */
typedef bool (*inside_fn)(float x, float y, const float *p);
static void shape(const struct canvas *c, inside_fn inside, const float *p) {
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) n += inside(x + (sx + .5f) / 4, y + (sy + .5f) / 4, p);
            uint8_t v = (uint8_t)(n * 255 / 16);
            if (c->m[y * c->w + x] < v) c->m[y * c->w + x] = v;
        }
}
static float absf(float v) { return v < 0 ? -v : v; }
static float sqrtf_(float v) { if (v <= 0) return 0; float x = v > 1 ? v : 1; for (int i = 0; i < 12; i++) x = .5f * (x + v / x); return x; }

/* --- box drawing ----------------------------------------------------------------- *
 * A single line of thickness t sits at (size - t) / 2 across the cell, the
 * same in every cell, so lines continue cell to cell. A double line is two
 * light lines with a light line's gap: at (size - 3l) / 2 and two l further.
 * Where arms meet, each line runs as far as the lines it joins. */

struct box { const struct canvas *c; int w, h, l, heavy; int up, right, down, left; };
static int thick(const struct box *b, int weight) { return weight == 2 ? b->heavy : b->l; }
/* single band start across a size, for thickness t */
static int band(int size, int t) { return (size - t) / 2; }

/* how far a horizontal line ends/starts against the vertical arms, at x */
static void single_extent(const struct box *b, bool horizontal, int *near_end, int *far_end) {
    /* the perpendicular arms' band: its start and end across this line */
    int a = horizontal ? b->up : b->left, z = horizontal ? b->down : b->right;
    int size = horizontal ? b->w : b->h, l = b->l;
    int d0 = band(size, 3 * l);
    if (a == 3 || z == 3) {
        /* a double perpendicular: stop at its near line, or cross to the far
         * one when it does not go straight through (a corner or a tee end) */
        bool through = a == 3 && z == 3;
        *near_end = through ? d0 + l : d0 + 3 * l;
        *far_end = through ? d0 + 2 * l : d0;
        return;
    }
    int t = 0;
    if (a) t = thick(b, a);
    if (z && thick(b, z) > t) t = thick(b, z);
    if (!t) {                          /* nothing across: this line's own width */
        int own = horizontal ? (b->left ? b->left : b->right) : (b->up ? b->up : b->down);
        t = thick(b, own == 3 ? 1 : own);
    }
    int s0 = band(size, t);
    *near_end = s0 + t;                /* the arm from the low side runs to here */
    *far_end = s0;                     /* the arm from the high side starts here */
}
static void single_arm(const struct box *b, bool horizontal, bool low, int weight) {
    int t = thick(b, weight), across = horizontal ? b->h : b->w, along = horizontal ? b->w : b->h;
    int c0 = band(across, t), near_end, far_start;
    single_extent(b, horizontal, &near_end, &far_start);
    int a0 = low ? 0 : far_start, a1 = low ? near_end : along;
    if (horizontal) fill(b->c, a0, c0, a1, c0 + t, 255);
    else fill(b->c, c0, a0, c0 + t, a1, 255);
}
/* One line of a double arm. `side` picks the line: 0 toward the low side of
 * the cross axis (up for a horizontal arm, left for a vertical one), 1 the
 * other. */
static void double_line(const struct box *b, bool horizontal, bool low, int side) {
    int l = b->l, across = horizontal ? b->h : b->w, along = horizontal ? b->w : b->h;
    int c0 = band(across, 3 * l) + (side ? 2 * l : 0);
    /* the perpendicular arm on this line's side, and on the other side */
    int mine = horizontal ? (side ? b->down : b->up) : (side ? b->right : b->left);
    int other = horizontal ? (side ? b->up : b->down) : (side ? b->left : b->right);
    int opposite = horizontal ? (low ? b->right : b->left) : (low ? b->down : b->up);
    int d0 = band(along, 3 * l), mid = along / 2;
    int end;                                       /* how far toward the far side the line runs */
    if (mine == 3) end = low ? d0 + l : d0 + 2 * l;                 /* inner corner */
    else if (opposite == 3 && !mine) end = mid;                     /* straight through, meeting its other half */
    else if (other == 3) end = low ? d0 + 3 * l : d0;               /* outer corner */
    else if (mine || other) {                                       /* a single line across */
        int t = mine ? thick(b, mine) : thick(b, other);
        if (mine && other) { int t2 = thick(b, other); if (t2 > t) t = t2; }
        int s0 = band(along, t);
        end = low ? s0 + t : s0;
    } else end = mid;
    int a0 = low ? 0 : end, a1 = low ? end : along;
    if (low && opposite == 3 && !mine && !other) a1 = mid + 1;       /* overlap the join */
    if (horizontal) fill(b->c, a0, c0, a1, c0 + l, 255);
    else fill(b->c, c0, a0, c0 + l, a1, 255);
}
static void arm(const struct box *b, bool horizontal, bool low, int weight) {
    if (!weight) return;
    if (weight == 3) { double_line(b, horizontal, low, 0); double_line(b, horizontal, low, 1); }
    else single_arm(b, horizontal, low, weight);
}
static void dashes(const struct canvas *c, bool horizontal, int n, int t) {
    int along = horizontal ? c->w : c->h, across = horizontal ? c->h : c->w;
    int c0 = band(across, t);
    for (int i = 0; i < n; i++) {
        int a0 = along * i / n, a1 = along * (i + 1) / n;
        int gap = (a1 - a0) / 3 > 0 ? (a1 - a0) / 3 : 1;
        a1 -= gap;
        if (a1 <= a0) a1 = a0 + 1;
        if (horizontal) fill(c, a0, c0, a1, c0 + t, 255);
        else fill(c, c0, a0, c0 + t, a1, 255);
    }
}
/* p: center x, y, radius, half thickness, quadrant signs sx, sy */
static bool in_arc(float x, float y, const float *p) {
    float dx = x - p[0], dy = y - p[1];
    if (dx * p[4] > 0 || dy * p[5] > 0) return false;
    return absf(sqrtf_(dx * dx + dy * dy) - p[2]) <= p[3];
}
static void arc(const struct canvas *c, uint32_t cp, int l) {
    float xc = (float)band(c->w, l) + l / 2.0f, yc = (float)band(c->h, l) + l / 2.0f;
    bool right = cp == 0x256d || cp == 0x2570, down = cp == 0x256d || cp == 0x256e;
    float rx = right ? c->w - xc : xc, ry = down ? c->h - yc : yc;
    float r = rx < ry ? rx : ry;
    float p[6] = { xc + (right ? r : -r), yc + (down ? r : -r), r, l / 2.0f, right ? 1.0f : -1.0f, down ? 1.0f : -1.0f };
    shape(c, in_arc, p);
    /* the straight parts beyond the curve, to the cell edges */
    int x0 = band(c->w, l), y0 = band(c->h, l);
    if (down) fill(c, x0, (int)(yc + r), x0 + l, c->h, 255); else fill(c, x0, 0, x0 + l, (int)(yc - r + .5f), 255);
    if (right) fill(c, (int)(xc + r), y0, c->w, y0 + l, 255); else fill(c, 0, y0, (int)(xc - r + .5f), y0 + l, 255);
}
/* p: x0 y0 x1 y1 (a segment), half thickness */
static bool in_segment(float x, float y, const float *p) {
    float dx = p[2] - p[0], dy = p[3] - p[1];
    float len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - p[0]) * dx + (y - p[1]) * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ex = p[0] + t * dx - x, ey = p[1] + t * dy - y;
    return ex * ex + ey * ey <= p[4] * p[4];
}
static void segment(const struct canvas *c, float x0, float y0, float x1, float y1, float half) {
    float p[5] = { x0, y0, x1, y1, half };
    shape(c, in_segment, p);
}
static void box_drawing(const struct canvas *c, uint32_t cp, int l) {
    const char *a = box[cp - 0x2500];
    int kind = kind_of(cp);
    if (kind == ARC) { arc(c, cp, l); return; }
    if (kind == DIAGONAL) {
        float half = l * .6f;
        if (cp != 0x2572) segment(c, (float)c->w, 0, 0, (float)c->h, half);
        if (cp != 0x2571) segment(c, 0, 0, (float)c->w, (float)c->h, half);
        return;
    }
    struct box b = { c, c->w, c->h, l, 2 * l, a[0] - '0', a[1] - '0', a[2] - '0', a[3] - '0' };
    if (kind != PLAIN) {
        int n = kind == DASH2 ? 2 : kind == DASH3 ? 3 : 4, t = thick(&b, b.right ? b.right : b.up);
        dashes(c, b.right != 0, n, t);
        return;
    }
    arm(&b, true, true, b.left);
    arm(&b, true, false, b.right);
    arm(&b, false, true, b.up);
    arm(&b, false, false, b.down);
}

/* --- block elements ---------------------------------------------------------------- */

static void blocks(const struct canvas *c, uint32_t cp) {
    int w = c->w, h = c->h;
#define EIGHTH(size, n) (((size) * (n) + 4) / 8)
    if (cp == 0x2580) fill(c, 0, 0, w, EIGHTH(h, 4), 255);
    else if (cp >= 0x2581 && cp <= 0x2588) fill(c, 0, h - EIGHTH(h, cp - 0x2580), w, h, 255);
    else if (cp >= 0x2589 && cp <= 0x258f) fill(c, 0, 0, EIGHTH(w, 0x2590 - cp), h, 255);
    else if (cp == 0x2590) fill(c, w - EIGHTH(w, 4), 0, w, h, 255);
    else if (cp >= 0x2591 && cp <= 0x2593) fill(c, 0, 0, w, h, (uint8_t)(64 * (cp - 0x2590)));
    else if (cp == 0x2594) fill(c, 0, 0, w, EIGHTH(h, 1), 255);
    else if (cp == 0x2595) fill(c, w - EIGHTH(w, 1), 0, w, h, 255);
    else {
        /* quadrants: bits upper-left 1, upper-right 2, lower-left 4, lower-right 8 */
        static const uint8_t q[10] = { 4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8 };
        uint8_t bits = q[cp - 0x2596];
        int mx = w / 2, my = h / 2;
        if (bits & 1) fill(c, 0, 0, mx, my, 255);
        if (bits & 2) fill(c, mx, 0, w, my, 255);
        if (bits & 4) fill(c, 0, my, mx, h, 255);
        if (bits & 8) fill(c, mx, my, w, h, 255);
    }
#undef EIGHTH
}

/* --- braille: eight dots, two columns of four ----------------------------------------- */

static bool in_dot(float x, float y, const float *p) {
    float dx = x - p[0], dy = y - p[1];
    return dx * dx + dy * dy <= p[2] * p[2];
}
static void braille(const struct canvas *c, uint32_t cp) {
    static const int col[8] = { 0, 0, 0, 1, 1, 1, 0, 1 }, row[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
    unsigned bits = cp - 0x2800;
    float r = (c->w / 4.0f < c->h / 8.0f ? c->w / 4.0f : c->h / 8.0f) * .8f;
    if (r < .7f) r = .7f;
    for (int i = 0; i < 8; i++) if (bits & (1u << i)) {
        float p[3] = { c->w * (col[i] ? .75f : .25f), c->h * (row[i] * 2 + 1) / 8.0f, r };
        shape(c, in_dot, p);
    }
}

/* --- Powerline: separators the height of the line ---------------------------------------- */

/* p: w, h, which */
static bool in_powerline(float x, float y, const float *p) {
    float w = p[0], h = p[1], u = x / w, v = y / h;
    switch ((int)p[2]) {
    case 0xb0: return u <= 1 - absf(2 * v - 1);                  /* right-pointing triangle */
    case 0xb2: return u >= absf(2 * v - 1);                      /* left-pointing */
    case 0xb4: { float dv = 2 * v - 1; return u * u + dv * dv <= 1; }          /* right half disc */
    case 0xb6: { float du = 1 - u, dv = 2 * v - 1; return du * du + dv * dv <= 1; }
    case 0xb8: return u <= v;                                     /* lower-left triangle */
    case 0xba: return u >= 1 - v;                                 /* lower-right */
    case 0xbc: return u <= 1 - v;                                 /* upper-left */
    case 0xbe: return u >= v;                                     /* upper-right */
    default: return false;
    }
}
static bool in_ring(float x, float y, const float *p) {
    /* p: center x, y, rx, ry, half thickness, half side (+1 right, -1 left) */
    float dx = x - p[0], dy = y - p[1];
    if (dx * p[5] < 0) return false;
    float nx = dx / p[2], ny = dy / p[3], d = sqrtf_(nx * nx + ny * ny);
    float scale = p[2] < p[3] ? p[2] : p[3];
    return absf(d - 1) * scale <= p[4];
}
static void powerline(const struct canvas *c, uint32_t cp, int l) {
    float w = (float)c->w, h = (float)c->h, half = l * .6f;
    switch (cp & 0xff) {
    case 0xb1: segment(c, 0, 0, w, h / 2, half); segment(c, w, h / 2, 0, h, half); return;
    case 0xb3: segment(c, w, 0, 0, h / 2, half); segment(c, 0, h / 2, w, h, half); return;
    case 0xb5: case 0xb7: {
        bool right = (cp & 0xff) == 0xb5;
        float p[6] = { right ? 0 : w, h / 2, w - half, h / 2 - half, half, right ? 1.0f : -1.0f };
        shape(c, in_ring, p);
        return;
    }
    case 0xb9: case 0xbf: segment(c, 0, 0, w, h, half); return;
    case 0xbb: case 0xbd: segment(c, 0, h, w, 0, half); return;
    default: {
        float p[3] = { w, h, (float)(cp & 0xff) };
        shape(c, in_powerline, p);
    }
    }
}

bool term_glyph_draw(uint32_t cp, uint8_t *mask, int w, int h, int line) {
    if (w <= 0 || h <= 0) return false;
    memset(mask, 0, (size_t)w * h);
    if (!term_glyph_is_procedural(cp)) return false;
    if (line < 1) line = 1;
    struct canvas c = { mask, w, h };
    if (cp <= 0x257f) box_drawing(&c, cp, line);
    else if (cp <= 0x259f) blocks(&c, cp);
    else if (cp >= 0x2800 && cp <= 0x28ff) braille(&c, cp);
    else powerline(&c, cp, line);
    return true;
}
