/* ==========================================================================
 * wcwidth.c -- the cell width table both the editor and the terminal use.
 * See wcwidth.h for why there is exactly one.
 * ========================================================================== */
#include "wcwidth.h"

int wcwidth_cp(uint32_t c) {
    if (!c || c < 32 || (c >= 0x7f && c < 0xa0)) return -1;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff) ||
        (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) ||
        c == 0x200d || (c >= 0xe0100 && c <= 0xe01ef)) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || c == 0x2329 || c == 0x232a || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe19) ||
        (c >= 0xfe30 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
        (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd)) return 2;
    return 1;
}

int utf8_decode(const char *s, int n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (n <= 0) { *cp = 0xfffd; return 1; }
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int len = u[0] >= 0xf0 && u[0] < 0xf5 ? 4 : u[0] >= 0xe0 ? 3 : u[0] >= 0xc2 && u[0] < 0xe0 ? 2 : 0;
    if (!len || len > n) { *cp = 0xfffd; return 1; }
    uint32_t c = u[0] & (len == 2 ? 0x1f : len == 3 ? 0x0f : 0x07);
    for (int i = 1; i < len; i++) {
        if ((u[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; }
        c = c << 6 | (u[i] & 0x3f);
    }
    static const uint32_t least[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (c < least[len] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { *cp = 0xfffd; return 1; }
    *cp = c;
    return len;
}
