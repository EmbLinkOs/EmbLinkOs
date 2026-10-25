/* display_arrange_test.c -- the arrangement Settings edits and the desktop
 * restores: from what is on screen, back to coordinates, primary changes,
 * displays joining and leaving, mirroring, refusals with the kernel's reasons,
 * and saved arrangements that come back only for the monitors they were made
 * for. Runs the kernel's own display/layout.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display_arrange.h"

static void mode(struct embk_output *o, uint32_t w, uint32_t h, int preferred) {
    o->modes[o->mode_count++] = (struct embk_monitor_mode){ w, h, 60000, preferred ? EMBK_MONITOR_PREFERRED : 0 };
}

/* A laptop panel (primary, 1920x1080) and a 2560x1440 monitor on its right. */
static struct embk_display_layout desk(void) {
    struct embk_display_layout d;
    memset(&d, 0, sizeof d);
    d.version = EMBK_DISPLAY_LAYOUT_VERSION;
    d.flags = EMBK_LAYOUT_MULTI;
    d.count = 3;
    d.generation = 7;
    struct embk_output *a = &d.outputs[0], *b = &d.outputs[1], *c = &d.outputs[2];
    strcpy(a->name, "eDP-1"); strcpy(a->model, "LGD 15.6"); memcpy(a->manufacturer, "LGD", 4);
    a->width_mm = 344; a->height_mm = 194;
    a->flags = EMBK_OUTPUT_CONNECTED | EMBK_OUTPUT_ENABLED | EMBK_OUTPUT_PRIMARY | EMBK_OUTPUT_EDID | EMBK_OUTPUT_BUILTIN;
    a->width = 1920; a->height = 1080;
    mode(a, 1920, 1080, 1); mode(a, 1280, 720, 0); mode(a, 1024, 768, 0);
    strcpy(b->name, "HDMI-1"); strcpy(b->model, "DELL U2719D"); memcpy(b->manufacturer, "DEL", 4);
    b->width_mm = 597; b->height_mm = 336;
    b->flags = EMBK_OUTPUT_CONNECTED | EMBK_OUTPUT_ENABLED | EMBK_OUTPUT_EDID;
    b->x = 1920; b->width = 2560; b->height = 1440;
    mode(b, 2560, 1440, 1); mode(b, 1920, 1080, 0); mode(b, 1280, 720, 0);
    strcpy(c->name, "DP-1");                                     /* nothing plugged in */
    return d;
}

/* What the kernel would show for request r, as the next layout. */
static void show(struct embk_display_layout *d, const struct embk_layout_request *r) {
    d->flags = EMBK_LAYOUT_MULTI | (r->flags & EMBK_LAYOUT_MIRROR);
    d->generation++;
    for (unsigned i = 0; i < d->count; i++) {
        struct embk_output *o = &d->outputs[i];
        o->flags &= ~(EMBK_OUTPUT_ENABLED | EMBK_OUTPUT_PRIMARY);
        o->x = o->y = 0; o->width = o->height = 0;
        if (!r->out[i].enabled) continue;
        o->flags |= EMBK_OUTPUT_ENABLED | (r->out[i].primary ? EMBK_OUTPUT_PRIMARY : 0);
        o->x = r->out[i].x; o->y = r->out[i].y; o->width = r->out[i].width; o->height = r->out[i].height;
    }
}

int main(void) {
    struct embk_display_layout d = desk();
    struct arrange a;
    struct embk_layout_request r;

    /* What is on screen, described and rebuilt exactly. */
    arrange_from_layout(&d, &a);
    assert(a.count == 3 && !a.mirror && a.out[0].primary && a.out[1].enabled && !a.out[2].enabled);
    assert(a.out[1].side == ARRANGE_RIGHT && a.out[1].order == 0 && a.out[1].offset == 0);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && !arrange_differs(&d, &r));
    assert(r.generation == 7 && r.count == 3 && r.out[1].x == 1920 && r.out[1].y == 0);

    /* The monitor moved to the left: the laptop is now at x 2560. */
    a.out[1].side = ARRANGE_LEFT;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && arrange_differs(&d, &r));
    assert(r.out[1].x == 0 && r.out[0].x == 2560 && r.out[0].y == 0);
    /* Above, centred on the laptop: offset -320 puts its middle over the panel's. */
    a.out[1].side = ARRANGE_ABOVE; a.out[1].offset = -320;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK);
    assert(r.out[1].x == 0 && r.out[1].y == 0 && r.out[0].x == 320 && r.out[0].y == 1440);
    /* Bottom-aligned on the right, described back the same way. */
    a.out[1].side = ARRANGE_RIGHT; a.out[1].offset = -360;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[0].y == 360 && r.out[1].y == 0);
    show(&d, &r);
    struct arrange back;
    arrange_from_layout(&d, &back);
    assert(back.out[1].side == ARRANGE_RIGHT && back.out[1].offset == -360);
    /* An offset that would leave no shared edge is pulled back to touching. */
    a.out[1].offset = 5000;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[1].y == 1079 && r.out[0].y == 0);

    /* Refusals carry the kernel's reasons. */
    d = desk(); arrange_from_layout(&d, &a);
    a.out[1].width = 3840; a.out[1].height = 2160;
    assert(arrange_request(&d, &a, &r) == ARRANGE_MODE);
    a.out[1].width = 2560; a.out[1].height = 1440;
    a.out[2].enabled = 1; a.out[2].width = 1920; a.out[2].height = 1080;
    assert(arrange_request(&d, &a, &r) == ARRANGE_DISCONNECTED);
    a.out[2].enabled = 0;
    a.out[0].primary = 0;
    assert(arrange_request(&d, &a, &r) == ARRANGE_PRIMARY);
    assert(arrange_error(ARRANGE_OVERLAP)[0] && arrange_error(ARRANGE_PRIMARY)[0]);

    /* A third monitor below the laptop that is wider than it, beside a tall
     * monitor on the right: they overlap, and that is said, not shown. */
    d = desk();
    struct embk_output *c = &d.outputs[2];
    strcpy(c->model, "BenQ"); memcpy(c->manufacturer, "BNQ", 4);
    c->flags = EMBK_OUTPUT_CONNECTED | EMBK_OUTPUT_EDID; mode(c, 2560, 1440, 1);
    arrange_from_layout(&d, &a);
    assert(arrange_set_enabled(&d, &a, 2, 1) == ARRANGE_OK && a.out[2].width == 2560 && a.out[2].side == ARRANGE_RIGHT);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[2].x == 4480);   /* joins outermost */
    a.out[2].side = ARRANGE_BELOW; a.out[2].order = 0;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OVERLAP);
    a.out[2].side = ARRANGE_LEFT;
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[2].x == 0 && r.out[0].x == 2560 && r.out[1].x == 4480);

    /* An adapter that lights two at most: a third is refused, with the reason. */
    d.max_enabled = 2;
    arrange_from_layout(&d, &a);
    assert(arrange_set_enabled(&d, &a, 2, 1) == ARRANGE_TOO_MANY && !a.out[2].enabled);
    a.out[2].enabled = 1; a.out[2].width = 2560; a.out[2].height = 1440; a.out[2].side = ARRANGE_LEFT;
    assert(arrange_request(&d, &a, &r) == ARRANGE_TOO_MANY && arrange_error(ARRANGE_TOO_MANY)[0]);
    d.max_enabled = 0;

    /* Make the big monitor primary: nothing moves, only the role. */
    d = desk(); arrange_from_layout(&d, &a);
    assert(arrange_set_primary(&d, &a, 1) == ARRANGE_OK && a.out[1].primary && !a.out[0].primary);
    assert(a.out[0].side == ARRANGE_LEFT);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[0].x == 0 && r.out[1].x == 1920 && r.out[1].primary);
    assert(arrange_set_primary(&d, &a, 2) == ARRANGE_PRIMARY);              /* not in use */

    /* Turning displays off: never the last; the primary's role moves on. */
    d = desk(); arrange_from_layout(&d, &a);
    assert(arrange_set_enabled(&d, &a, 0, 0) == ARRANGE_OK && !a.out[0].enabled && a.out[1].primary);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[1].x == 0 && r.out[1].y == 0);
    assert(arrange_set_enabled(&d, &a, 1, 0) == ARRANGE_NONE);
    /* A left-centre-right row loses its middle: the outer two close the gap. */
    d = desk(); c = &d.outputs[2];
    strcpy(c->model, "BenQ"); c->flags = EMBK_OUTPUT_CONNECTED | EMBK_OUTPUT_ENABLED; mode(c, 1280, 720, 1);
    c->width = 1280; c->height = 720;
    d.outputs[0].x = 1280; d.outputs[1].x = 3200;
    arrange_from_layout(&d, &a);
    assert(a.out[2].side == ARRANGE_LEFT && a.out[1].side == ARRANGE_RIGHT);
    assert(arrange_set_enabled(&d, &a, 0, 0) == ARRANGE_OK);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK);
    assert(r.out[2].enabled && r.out[1].enabled && !r.out[0].enabled);
    assert((r.out[2].x + 1280 == r.out[1].x) || (r.out[1].x + 2560 == r.out[2].x));

    /* Mirroring: the largest size both can show, the primary's own when it can. */
    d = desk(); arrange_from_layout(&d, &a);
    uint32_t w[16], h[16];
    assert(arrange_common_modes(&d, &a, w, h, 16) == 2 && w[0] == 1920 && h[0] == 1080 && w[1] == 1280);
    assert(arrange_set_mirror(&d, &a, 1) == ARRANGE_OK && a.mirror && a.out[1].width == 1920);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && (r.flags & EMBK_LAYOUT_MIRROR));
    assert(r.out[0].x == 0 && r.out[1].x == 0 && r.out[1].width == 1920);
    a.out[1].width = 1280; a.out[1].height = 720;
    assert(arrange_request(&d, &a, &r) == ARRANGE_MIRROR_SIZE);
    a.out[1].width = 1920; a.out[1].height = 1080;
    show(&d, &(struct embk_layout_request){ 0 });                         /* (reset, then show the mirror) */
    arrange_request(&d, &a, &r); show(&d, &r);
    arrange_from_layout(&d, &a);
    assert(a.mirror && a.out[0].width == 1920 && a.out[1].width == 1920);
    assert(arrange_set_mirror(&d, &a, 0) == ARRANGE_OK && !a.mirror && a.out[1].width == 2560);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK && r.out[1].x == 1920);
    /* Displays with no size in common cannot mirror. */
    d = desk(); d.outputs[1].mode_count = 0; mode(&d.outputs[1], 2560, 1440, 1);
    arrange_from_layout(&d, &a);
    assert(arrange_set_mirror(&d, &a, 1) == ARRANGE_MIRROR_SIZE && !a.mirror);

    /* Following a monitor: the big one moved from the right to the left. */
    d = desk();
    struct embk_display_layout moved = d;
    moved.outputs[1].x = 0; moved.outputs[0].x = 2560;
    int32_t fx, fy;
    assert(arrange_follow(&d, &moved, 3000, 500, &fx, &fy) == 1 && fx == -1920 && fy == 0);   /* on the monitor */
    assert(arrange_follow(&d, &moved, 100, 100, &fx, &fy) == 1 && fx == 2560);               /* on the panel */
    assert(arrange_follow(&d, &moved, 100, 1300, &fx, &fy) == 0);                            /* on neither */
    moved.outputs[1].flags &= ~EMBK_OUTPUT_ENABLED;
    assert(arrange_follow(&d, &moved, 3000, 500, &fx, &fy) == 0);                            /* its monitor is gone */
    /* Leaving a mirror, a point goes with the primary: the second monitor here. */
    struct embk_display_layout mir = d;
    mir.flags |= EMBK_LAYOUT_MIRROR;
    mir.outputs[1].x = 0; mir.outputs[1].width = 1920; mir.outputs[1].height = 1080;
    mir.outputs[0].flags &= ~EMBK_OUTPUT_PRIMARY; mir.outputs[1].flags |= EMBK_OUTPUT_PRIMARY;
    struct embk_display_layout ext = d;
    ext.outputs[0].flags &= ~EMBK_OUTPUT_PRIMARY; ext.outputs[1].flags |= EMBK_OUTPUT_PRIMARY;
    assert(arrange_follow(&mir, &ext, 100, 100, &fx, &fy) == 1 && fx == 1920);
    /* A resolution change on one monitor moves nothing. */
    struct embk_display_layout small = d;
    small.outputs[0].width = 1280; small.outputs[0].height = 720;
    assert(arrange_follow(&d, &small, 100, 100, &fx, &fy) == 1 && fx == 0 && fy == 0);

    /* Identity: the connector and the EDID; the set is order-free. */
    d = desk();
    uint32_t id0 = arrange_output_id(&d.outputs[0]), id1 = arrange_output_id(&d.outputs[1]);
    assert(id0 && id1 && id0 != id1);
    struct embk_display_layout other = d;
    strcpy(other.outputs[1].model, "DELL U2720Q");
    assert(arrange_output_id(&other.outputs[1]) != id1);
    struct embk_display_layout swapped = d;
    swapped.outputs[0] = d.outputs[1]; swapped.outputs[1] = d.outputs[0];
    assert(arrange_connected_set(&swapped) == arrange_connected_set(&d) && arrange_connected_set(&d));
    assert(arrange_connected(&d) == 2);

    /* Save the monitor on the left, the big one primary; restore it on the next boot. */
    d = desk(); arrange_from_layout(&d, &a);
    a.out[1].side = ARRANGE_LEFT; arrange_set_primary(&d, &a, 1);
    assert(arrange_request(&d, &a, &r) == ARRANGE_OK);
    struct embk_display_layout saved_on = d; show(&saved_on, &r);
    struct display_saved s;
    arrange_save(&saved_on, &s);
    assert(s.count == 2 && !s.mirror && s.out[0].id == id0 && s.out[1].primary && s.out[1].x == 0 && s.out[0].x == 2560);
    d = desk();                                                             /* the boot's own layout */
    assert(arrange_saved_request(&d, &s, &r) == 1);
    assert(r.out[0].x == 2560 && r.out[1].x == 0 && r.out[1].primary && !r.out[0].primary && r.generation == d.generation);
    show(&d, &r);
    assert(arrange_saved_request(&d, &s, &r) == 0);                         /* already showing it */
    /* Not for other monitors, or more of them, or fewer. */
    assert(arrange_saved_request(&other, &s, &r) == ARRANGE_MISMATCH);
    struct embk_display_layout three = desk();
    three.outputs[2].flags = EMBK_OUTPUT_CONNECTED; mode(&three.outputs[2], 1024, 768, 1);
    assert(arrange_saved_request(&three, &s, &r) == ARRANGE_MISMATCH);
    struct embk_display_layout alone = desk();
    alone.outputs[1].flags = 0;
    assert(arrange_saved_request(&alone, &s, &r) == ARRANGE_MISMATCH);
    /* A saved size the monitor no longer offers is refused, not forced. */
    struct display_saved stale = s;
    stale.out[1].width = 3840; stale.out[1].height = 2160;
    struct embk_display_layout fresh = desk();
    assert(arrange_saved_request(&fresh, &stale, &r) == ARRANGE_MODE);
    /* The same monitors enumerated in the other order still match. */
    assert(arrange_saved_request(&swapped, &s, &r) == 1 && r.out[0].primary && r.out[0].x == 0);
    struct display_saved none = { 0 };
    assert(arrange_saved_request(&d, &none, &r) == ARRANGE_MISMATCH);

    puts("Display arrangement: described from the screen and rebuilt exactly, sides/offsets/order, overlap and "
         "mode refusals, primary moves, displays joining and leaving, mirroring, identity, saved arrangements "
         "restored only for their monitors OK");
    return 0;
}
