/* frameclock_test.c -- the compositor's frame-rate policy (kernel/gfx/frameclock.c)
 * on the host: which rate a display gets for motion, for an app's request and
 * for content that asked nothing; what the governor and a power cap hold it
 * to; and the beat itself.  make frameclock-test -> exits 0 iff every claim holds. */
#include <stdint.h>
#include <stdio.h>
#include "gfx/frameclock.h"

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

static uint32_t rate(uint32_t refresh, uint32_t vmin, uint32_t vmax,
                     int motion, uint32_t request, uint32_t cost_us, uint32_t power) {
    struct fc_display d = { refresh, vmin, vmax };
    struct fc_inputs in = { motion, request, cost_us, power };
    return fc_rate_mhz(&d, &in);
}

int main(void) {
    printf("=== frameclock: the rate and the beat ===\n");
    /* a 60 Hz panel */
    CHECK(rate(60000, 0, 0, 1, 0, 0, 0) == 60000, "60 Hz: motion gets every refresh");
    CHECK(rate(60000, 0, 0, 0, 0, 0, 0) == 60000, "60 Hz: content that asked nothing gets the default, 60");
    CHECK(rate(60000, 0, 0, 0, 24000, 0, 0) == 30000, "60 Hz: a 24 fps video gets 30, not 20 -- never less than it asked");
    CHECK(rate(60000, 0, 0, 0, 1000, 0, 0) == 7500, "60 Hz: a once-a-second clock gets the slowest beat, refresh/8");
    CHECK(rate(0, 0, 0, 1, 0, 0, 0) == 60000, "a display that does not say is taken as 60 Hz");

    /* a 120 Hz panel -- the ProMotion case */
    CHECK(rate(120000, 0, 0, 1, 0, 0, 0) == 120000, "120 Hz: motion under a hand gets 120");
    CHECK(rate(120000, 0, 0, 0, 0, 0, 0) == 60000, "120 Hz: content that asked nothing gets 60");
    CHECK(rate(120000, 0, 0, 0, FC_MAX, 0, 0) == 120000, "120 Hz: an app asking for all of it gets 120");
    CHECK(rate(120000, 0, 0, 0, 24000, 0, 0) == 24000, "120 Hz: a 24 fps video gets exactly 24 (120/5)");
    CHECK(rate(120000, 0, 0, 0, 90000, 0, 0) == 120000, "120 Hz: 90 asked is 120 given -- the slowest whole fraction at or above it");
    CHECK(rate(120000, 0, 0, 0, 50000, 0, 0) == 60000, "120 Hz: 50 asked is 60 given");

    /* the governor and the power cap */
    CHECK(rate(120000, 0, 0, 1, 0, 6000, 0) == 120000, "a 6 ms frame keeps 120 (three quarters of 8.3 ms is 6.25)");
    CHECK(rate(120000, 0, 0, 1, 0, 7000, 0) == 60000, "a 7 ms frame cannot keep 120: an even 60 instead of a ragged 120");
    CHECK(rate(60000, 0, 0, 1, 0, 23000, 0) == 30000, "a 23 ms frame (TCG) holds 60 Hz to 30, a fraction it can keep");
    CHECK(rate(60000, 0, 0, 1, 0, 200000, 0) == 7500, "however slow, never below refresh/8");
    CHECK(rate(120000, 0, 0, 1, 0, 0, 60000) == 60000, "a power cap of 60 holds motion at 60");
    CHECK(rate(120000, 0, 0, 0, 24000, 0, 60000) == 24000, "a power cap leaves a slower request alone");

    /* a VRR display, 48-120 Hz */
    CHECK(rate(120000, 48000, 120000, 1, 0, 0, 0) == 120000, "VRR: motion gets the top");
    CHECK(rate(120000, 48000, 120000, 0, 90000, 0, 0) == 90000, "VRR: 90 asked is exactly 90");
    CHECK(rate(120000, 48000, 120000, 0, 24000, 0, 0) == 48000, "VRR: below the range is its bottom (the panel repeats frames)");
    CHECK(rate(120000, 48000, 120000, 0, 0, 0, 0) == 60000, "VRR: content that asked nothing gets 60");
    CHECK(rate(120000, 48000, 120000, 1, 0, 9000, 0) == 83333, "VRR: a 9 ms frame keeps 83.3 Hz, any rate the governor allows");

    /* periods */
    CHECK(fc_period_ns(60000) == 16666666 && fc_period_ns(120000) == 8333333 && fc_period_ns(24000) == 41666666,
          "periods: 16.67 ms, 8.33 ms, 41.67 ms");

    /* the beat */
    const uint64_t P = 16666666;
    CHECK(fc_next_due(0, P, 5000000000ull) == 5000000000ull, "still for longer than a period: the frame is due now");
    CHECK(fc_next_due(1000000000ull, P, 1000000000ull + 3000000) == 1000000000ull + P,
          "in motion: due a period after the last was due, not after it finished");
    uint64_t due = 1000000000ull, drift = 0;
    for (int i = 0; i < 600; i++) {                     /* ten seconds of frames costing 4 ms each */
        uint64_t finished = due + 4000000;
        due = fc_next_due(due, P, finished);
    }
    drift = due - (1000000000ull + 600 * P);
    CHECK(drift == 0, "600 frames later the beat is exactly where it was due -- no drift from frame cost");
    CHECK(fc_next_due(due, 8333333, due + 2000000) == due + 8333333, "the period can change between frames: 60 to 120");

    /* the reservation */
    CHECK(fc_budget_ms(16, 0, 0) == 1, "reservation: nothing measured yet asks 1 ms, not half the period");
    CHECK(fc_budget_ms(16, 2600, 0) == 4, "reservation: a 2.6 ms frame asks 4 ms (half again, rounded up)");
    CHECK(fc_budget_ms(8, 1900, 1100) == 4, "reservation: the timer lead it spins is part of the frame");
    CHECK(fc_budget_ms(33, 23000, 0) == 16, "reservation: a 23 ms frame (TCG) is held to half its period");
    CHECK(fc_budget_ms(1, 5000, 0) == 1, "reservation: never 0, whatever the period");

    printf("=== frameclock: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
