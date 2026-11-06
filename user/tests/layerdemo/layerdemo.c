/* layerdemo.c -- a layer the compositor moves while its application sleeps.
 *
 * The witness for the render server (user/lib/embk_layer.h). It opens a
 * window, paints it once, and paints one layer once: a card with rounded,
 * anti-aliased corners and a translucent body, reaching below the window's
 * frame the way a menu does. Then, every `period` ms, it sends the card in --
 * from 0.94 of its size, transparent and 8 px up, to full size, opaque and in
 * place -- or out again, with ONE embk_layer_set, and goes back to sleep. It
 * never draws another frame and never presents its window again: whatever
 * moves on screen is the compositor's doing.
 *
 * It also polls its input, slowly, and says where a press landed in its own
 * coordinates ("layerdemo: press x y"). A press on the part of the card that
 * hangs below the window -- where nothing but the layer is -- sends the card
 * away at once, so a test can see the press arrived (perf windows' layer line).
 *
 *   layerdemo [period_ms] [cycles]       default 1500 ms, 6 cycles (0: forever)
 *
 * Exit 0 when every call succeeded, 1 when the kernel has no layers, 2 for
 * any other failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "embk.h"

/* a kernel without the call answers its empty slot with -EINVAL (22); one
 * that has it and cannot answers -ENOSYS (38) */
#define LAYER_NOSYS 38

#define WW 360
#define WH 220
#define CW 240                 /* the card */
#define CH 180
#define CX 60                  /* where it stands, window-content-local: */
#define CY 120                 /* its lower 100 px hang below the window */
#define CR 14                  /* its corner radius */

static void say(const char *s) { embk_puts(1, s); }

/* premultiplied 0xAARRGGBB, alpha a, colour (r, g, b) */
static uint32_t pm(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
}

/* coverage of pixel (x, y) by a rounded rect of w x h, radius r, 0..255,
 * 4 x 4 supersampled -- integers only */
static uint32_t cover(int x, int y, int w, int h, int r) {
    int n = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int px = x * 4 + sx * 1 + 0, py = y * 4 + sy;   /* quarter pixels */
            int qx = px < r * 4 ? r * 4 - px : px >= (w - r) * 4 ? px - (w - r) * 4 + 1 : 0;
            int qy = py < r * 4 ? r * 4 - py : py >= (h - r) * 4 ? py - (h - r) * 4 + 1 : 0;
            if (qx * qx + qy * qy <= r * 4 * r * 4) n++;
        }
    return (uint32_t)(n * 255 / 16);
}

int main(int argc, char **argv) {
    int period = argc > 1 ? atoi(argv[1]) : 1500, cycles = argc > 2 ? atoi(argv[2]) : 6;
    if (period < 200) period = 200;
    uint32_t *win_px = 0;
    int win = embk_win_create_shared(WW, WH, 380, 200, "Layer demo", (void **)&win_px);
    if (win < 0 || !win_px) { say("layerdemo: no window\n"); return 2; }
    for (int y = 0; y < WH; y++)
        for (int x = 0; x < WW; x++)
            win_px[y * WW + x] = 0xff000000u | (uint32_t)(0x20 + y / 8) << 16 | 0x283000u | (uint32_t)(0x40 + x / 8);
    embk_win_present(win, win_px, WW, WH);

    uint64_t va = 0;
    int64_t id = embk_layer_create(win, CW, CH, &va);
    if (id == -LAYER_NOSYS || id == -EMBK_EINVAL) {
        char b[80]; snprintf(b, sizeof b, "layerdemo: no layers (%d)\n", (int)id); say(b);
        return 1;
    }
    if (id < 0 || !va) { say("layerdemo: layer refused\n"); return 2; }
    uint32_t *card = (uint32_t *)(uintptr_t)va;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            uint32_t c = cover(x, y, CW, CH, CR);
            /* a light, 88%-opaque body with a stripe band, so scaling shows */
            uint32_t a = c * 224 / 255, band = (y / 12) & 1;
            card[y * CW + x] = c ? pm(a, band ? 0xf2 : 0xe4, band ? 0xf4 : 0xe8, 0xff) : 0;
        }

    struct embk_layer_op op[2];
    memset(op, 0, sizeof op);
    /* where it starts: out of sight, small, a little high -- the "from" */
    op[0].id = (uint32_t)id; op[0].set = op[0].snap = EMBK_LP_POS | EMBK_LP_OPACITY | EMBK_LP_SCALE;
    op[0].set |= EMBK_LP_LOOK;
    op[0].x = CX; op[0].y = CY - 8; op[0].opacity = 0; op[0].scale_milli = 940;
    op[0].anchor_x_milli = 500; op[0].anchor_y_milli = 0;
    op[0].radius = CR; op[0].shadow_dy = 8; op[0].shadow_blur = 24; op[0].shadow_alpha = 60;
    op[0].flags = EMBK_LF_HIT;
    if (embk_layer_set(win, op, 1, EMBK_LS_AT_PRESENT) < 0 ||
        embk_layer_present(win, (uint32_t)id, 0, 0, CW, CH) < 0) { say("layerdemo: set refused\n"); return 2; }
    say("layerdemo: ready\n");

    int shown = 0;
    uint64_t next = embk_uptime_ms() + (uint64_t)period;
    for (int k = 0; cycles == 0 || k < 2 * cycles; ) {
        struct embk_win_input in;
        if (embk_win_input(&in) >= 0 && in.focused && (in.buttons & EMBK_MOUSE_LEFT)) {
            static uint32_t last;
            if (in.when != last) {
                last = in.when;
                char b[64]; snprintf(b, sizeof b, "layerdemo: press %d %d\n", (int)in.x, (int)in.y); say(b);
                /* a press ON THE CARD -- below the window, where only the
                 * layer is -- sends it away at once: what a test watches */
                if (shown && in.y >= WH && in.x >= CX && in.x < CX + CW) next = embk_uptime_ms();
            }
        }
        if (embk_uptime_ms() < next) { embk_sleep_ms(200); continue; }   /* a slow app */
        next += (uint64_t)period;
        shown = !shown; k++;
        struct embk_layer_op go; memset(&go, 0, sizeof go);
        go.id = (uint32_t)id; go.set = EMBK_LP_POS | EMBK_LP_OPACITY | EMBK_LP_SCALE;
        go.x = CX; go.y = shown ? CY : CY - 8; go.opacity = shown ? 255 : 0;
        go.scale_milli = shown ? 1000 : 940; go.anchor_x_milli = 500; go.anchor_y_milli = 0;
        go.move.response_ms = go.fade.response_ms = go.grow.response_ms = 350;
        go.move.damping_milli = go.fade.damping_milli = go.grow.damping_milli = 1000;
        if (embk_layer_set(win, &go, 1, 0) < 0) { say("layerdemo: set refused\n"); return 2; }
        say(shown ? "layerdemo: in\n" : "layerdemo: out\n");
    }
    say("layerdemo: done\n");
    return 0;
}
