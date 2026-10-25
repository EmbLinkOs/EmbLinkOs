/* display_layout_test.c -- several outputs, one desktop: validation, the
 * placement of a new monitor, normalisation, pointer bounds, and where windows
 * go when a monitor is unplugged. */
#include <assert.h>
#include <stdio.h>
#include "display/layout.h"

static struct layout two(void)
{
    struct layout l = { 2, 0, { { 1, 1, 1, 0, 0, 1920, 1080 }, { 1, 1, 0, 1920, 0, 2560, 1440 } } };
    return l;
}

int main(void)
{
    uint32_t w = 0, h = 0;
    struct layout l = two();
    assert(layout_validate(&l, &w, &h) == LAYOUT_OK && w == 4480 && h == 1440);

    /* Refusals, each for its reason. */
    l = two(); l.out[1].x = 1900;
    assert(layout_validate(&l, 0, 0) == LAYOUT_OVERLAP);
    l = two(); l.out[1].x = 2000;
    assert(layout_validate(&l, 0, 0) == LAYOUT_DETACHED);           /* a gap the pointer cannot cross */
    l = two(); l.out[1].x = 1920; l.out[1].y = 1080;
    assert(layout_validate(&l, 0, 0) == LAYOUT_DETACHED);           /* corner to corner only */
    l = two(); l.out[1].primary = 1;
    assert(layout_validate(&l, 0, 0) == LAYOUT_PRIMARY);
    l = two(); l.out[0].enabled = 0; l.out[1].enabled = 0;
    assert(layout_validate(&l, 0, 0) == LAYOUT_NONE);
    l = two(); l.out[1].connected = 0;
    assert(layout_validate(&l, 0, 0) == LAYOUT_DISCONNECTED);
    l = two(); l.out[1].w = 3840; l.out[1].h = 2160; l.out[0].w = 3840; l.out[0].h = 2160; l.out[1].x = 3840;
    assert(layout_validate(&l, &w, &h) == LAYOUT_OK && w == 7680);   /* two 4K: 16.59 M pixels, allowed */
    l.out[0].w = 5120; l.out[0].h = 2880; l.out[1].x = 5120;
    assert(layout_validate(&l, 0, 0) == LAYOUT_TOO_LARGE);          /* 8960 wide: past a plane's 8192 */
    l = two(); l.mirror = 1;
    assert(layout_validate(&l, 0, 0) == LAYOUT_MIRROR_SIZE);
    l.out[1].w = 1920; l.out[1].h = 1080;
    assert(layout_validate(&l, &w, &h) == LAYOUT_OK && w == 1920 && h == 1080);

    /* An external monitor to the LEFT of the laptop: negative until normalised. */
    l = two(); l.out[1].x = -2560; l.out[1].y = -360;
    assert(layout_validate(&l, &w, &h) == LAYOUT_OK && w == 4480 && h == 1440);
    layout_normalize(&l);
    assert(l.out[1].x == 0 && l.out[1].y == 0 && l.out[0].x == 2560 && l.out[0].y == 360);

    /* A monitor plugged in goes to the right of everything, level with the primary. */
    struct layout three = { 3, 0, { { 1, 1, 1, 0, 100, 1920, 1080 }, { 1, 1, 0, 1920, 0, 1280, 1024 },
                                    { 1, 1, 0, 0, 0, 1024, 768 } } };
    layout_place_right(&three, 2);
    assert(three.out[2].x == 3200 && three.out[2].y == 100);
    assert(layout_validate(&three, &w, &h) == LAYOUT_OK && w == 4224);

    /* Rectangles come primary first. */
    l = two(); l.out[0].primary = 0; l.out[1].primary = 1;
    struct layout_rect r[4];
    unsigned n = layout_rects(&l, r, 4);
    assert(n == 2 && r[0].x == 1920 && r[0].w == 2560 && r[1].x == 0);

    /* The pointer: on an output stays; in the dead corner below the shorter
     * output it goes to the nearest pixel that is shown. */
    l = two();
    n = layout_rects(&l, r, 4);
    assert(layout_rect_at(r, n, 100, 100) == 0 && layout_rect_at(r, n, 3000, 1300) == 1 && layout_rect_at(r, n, 100, 1300) == -1);
    int32_t x = 100, y = 1300;
    layout_clamp_point(r, n, &x, &y);
    assert(x == 100 && y == 1079);
    x = 1800; y = 1400;
    layout_clamp_point(r, n, &x, &y);
    assert(x == 1920 && y == 1400);                                 /* nearer the big one's edge */
    x = 5000; y = -50;
    layout_clamp_point(r, n, &x, &y);
    assert(x == 4479 && y == 0);

    /* Unplug the 2560: a window there moves to the laptop, keeping its offset. */
    struct layout_rect only[1] = { { 0, 0, 1920, 1080 } };
    x = 2100; y = 200;
    assert(layout_rehome(only, 1, &x, &y, 800, 600) == 1 && x == 180 && y == 200);
    x = 4000; y = 1300;
    assert(layout_rehome(only, 1, &x, &y, 800, 600) == 1 && x + 800 <= 1920 && y + 600 <= 1080 && x >= 0 && y >= 0);
    x = 100; y = 100;
    assert(layout_rehome(only, 1, &x, &y, 800, 600) == 0);          /* already on an output: untouched */
    puts("Display layout: extend and mirror validation, overlap/gap/corner/primary/size refusals, normalisation, "
         "new-monitor placement, pointer bounds, re-homing windows OK");
    return 0;
}
