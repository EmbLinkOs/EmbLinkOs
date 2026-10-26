/* kernel/gfx/frameclock.c -- see frameclock.h. No kernel state, no kernel
 * headers: the host test compiles this file as it is. */
#include "gfx/frameclock.h"

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

uint32_t fc_rate_mhz(const struct fc_display *d, const struct fc_inputs *in) {
    uint32_t refresh = d->refresh_mhz ? d->refresh_mhz : FC_UNKNOWN_MHZ;
    int vrr = d->vrr_max_mhz != 0;
    uint32_t top = vrr ? d->vrr_max_mhz : refresh;
    uint32_t bottom = vrr ? (d->vrr_min_mhz ? d->vrr_min_mhz : top / FC_MAX_DIVISOR)
                          : refresh / FC_MAX_DIVISOR;

    /* WHAT IS WANTED. Motion under a hand: everything the display has. An app
     * that asked: what it asked (FC_MAX is "as fast as you go"). Anything else
     * -- an app redrawing without asking -- the default, which is what every
     * desktop gives content that did not ask for more. */
    uint32_t want = FC_DEFAULT_MHZ;
    if (in->request_mhz) want = in->request_mhz == FC_MAX ? top : in->request_mhz;
    if (in->motion) want = top;

    /* WHAT CAN BE KEPT. The governor: a frame costing c is composed no faster
     * than the budget allows. Then the power cap. */
    uint32_t cap = top;
    if (in->frame_cost_us) {
        uint64_t gov = (uint64_t)FC_BUDGET_PERMILLE * 1000000ull / in->frame_cost_us;   /* mHz */
        cap = (uint32_t)(gov < cap ? gov : cap);
    }
    if (in->power_cap_mhz) cap = min_u32(cap, in->power_cap_mhz);

    if (vrr)                          /* any rate in range */
        return max_u32(min_u32(min_u32(want, cap), top), bottom);

    /* A FIXED DISPLAY: the slowest whole fraction of the refresh that gives
     * what is wanted -- 30 for a 24 fps video at 60 Hz, 24 at 120 -- if it
     * can be kept; else the fastest that can. Never slower than refresh/8. */
    uint32_t best = 0;
    for (uint32_t k = FC_MAX_DIVISOR; k >= 1; k--) {
        uint32_t r = refresh / k;
        if (r > cap) break;
        best = r;
        if (r >= want) return r;
    }
    return best ? best : bottom;
}

uint64_t fc_next_due(uint64_t last_due_ns, uint64_t period_ns, uint64_t now_ns) {
    if (now_ns >= last_due_ns + period_ns) return now_ns;
    return last_due_ns + period_ns;
}

uint32_t fc_budget_ms(uint32_t period_ms, uint32_t frame_cost_us, uint32_t lead_us) {
    uint64_t need_us = (uint64_t)frame_cost_us * 3 / 2 + lead_us;
    uint64_t ms = (need_us + 999) / 1000;
    uint32_t top = period_ms / 2 ? period_ms / 2 : 1;
    if (ms < 1) ms = 1;
    return ms > top ? top : (uint32_t)ms;
}

uint64_t fc_period_ns(uint32_t rate_mhz) {
    if (!rate_mhz) rate_mhz = FC_UNKNOWN_MHZ;
    return 1000000000000ull / rate_mhz;
}
