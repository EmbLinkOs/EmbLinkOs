/* vt_dump.c -- feed a recorded stream into the screen model and print what it
 * shows, for tools/golden_terminal.py to hold against a reference emulator.
 *
 *     vt_dump COLS ROWS stream.bin
 *
 * JSON on stdout: the size, the cursor, which screen is shown, the 256-colour
 * palette the model resolves indices with, the window title, and every cell of the shown screen
 * as [text, fg, bg, attributes, width] -- fg/bg -1 for "the profile's default",
 * otherwise 0xRRGGBB; attributes as letters (B bold, D dim, I italic, U<n>
 * underline style n, R inverse, H hidden, S strike, K blink, O overline, then
 * C<rrggbb> for an underline's own colour). A wide glyph's second cell is
 * null. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt.h"

static void json_text(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: vt_dump COLS ROWS stream.bin\n"); return 2; }
    int cols = atoi(argv[1]), rows = atoi(argv[2]);
    FILE *f = fopen(argv[3], "rb");
    if (!f) { perror(argv[3]); return 2; }
    static struct vt v;
    if (!vt_init(&v, cols, rows, NULL, NULL)) { fprintf(stderr, "vt_dump: out of memory\n"); return 3; }
    if (v.cols != cols || v.rows != rows) { fprintf(stderr, "vt_dump: the model cannot be %dx%d\n", cols, rows); return 3; }
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) vt_feed(&v, buf, n);
    fclose(f);
    const struct vt_screen *s = vt_active(&v);
    printf("{\"cols\":%d,\"rows\":%d,\"cursor\":[%d,%d],\"alternate\":%d,\"cursor_visible\":%d,\"palette\":[",
           v.cols, v.rows, s->cursor.x, s->cursor.y, v.alt, v.cursor_visible);
    for (unsigned i = 0; i < 256; i++) printf("%s%u", i ? "," : "", vt_rgb(&v, VT_COLOR_INDEX(i), true));
    printf("],\"title\":");
    json_text(v.title);
    printf(",\"cells\":[");
    for (int y = 0; y < v.rows; y++) {
        printf("%s[", y ? ",\n" : "\n");
        for (int x = 0; x < v.cols; x++) {
            const struct vt_cell *c = vt_view_cell(&v, y, x, 0);
            if (x) putchar(',');
            if (!c->width) { printf("null"); continue; }
            char text[64];
            vt_cell_text(&v, c, text);
            const struct vt_style *st = vt_style_of(&v, c);
            char attrs[32]; int a = 0;
            if (st->attrs & VT_BOLD) attrs[a++] = 'B';
            if (st->attrs & VT_DIM) attrs[a++] = 'D';
            if (st->attrs & VT_ITALIC) attrs[a++] = 'I';
            if (st->underline) a += snprintf(attrs + a, sizeof attrs - (size_t)a, "U%u", st->underline);
            if (st->attrs & VT_INVERSE) attrs[a++] = 'R';
            if (st->attrs & VT_HIDDEN) attrs[a++] = 'H';
            if (st->attrs & VT_STRIKE) attrs[a++] = 'S';
            if (st->attrs & VT_BLINK) attrs[a++] = 'K';
            if (st->attrs & VT_OVERLINE) attrs[a++] = 'O';
            if (st->underline && st->ul != VT_COLOR_DEFAULT)
                a += snprintf(attrs + a, sizeof attrs - (size_t)a, "C%06x", vt_rgb(&v, st->ul, true));
            attrs[a] = 0;
            putchar('['); json_text(text);
            printf(",%lld,%lld,\"%s\",%u]", st->fg == VT_COLOR_DEFAULT ? -1LL : (long long)vt_rgb(&v, st->fg, true),
                   st->bg == VT_COLOR_DEFAULT ? -1LL : (long long)vt_rgb(&v, st->bg, false), attrs, c->width);
        }
        putchar(']');
    }
    printf("\n]}\n");
    vt_free(&v);
    return 0;
}
