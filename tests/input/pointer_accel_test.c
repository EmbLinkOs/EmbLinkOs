/* pointer_accel_test.c -- the pointer's acceleration curve
 * (kernel/drivers/input/pointer_accel.c) on the host: 1:1 while aiming, more
 * across a sweep, nothing lost to rounding, the tracking speed scaling it.
 * make pointer-accel-test -> exits 0 iff every claim holds. */
#include <stdint.h>
#include <stdio.h>
#include "drivers/input/pointer_accel.h"

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

#define MS 1000000ull

/* `n` reports of (dx, dy) every `every_ns`, summed after acceleration -- the
 * first one of a motion included, which is taken 1:1 (see below) */
static int32_t g_last_x;
static void run(int n, int32_t dx, int32_t dy, uint64_t every_ns, uint32_t tracking, int32_t *sx, int32_t *sy) {
    struct ptr_accel a = { 0, 0, 0 };
    uint64_t t = 1000 * MS;
    *sx = *sy = 0;
    for (int i = 0; i < n; i++) {
        int32_t x = dx, y = dy;
        ptr_accel_apply(&a, &x, &y, t, tracking);
        *sx += x; *sy += y; g_last_x = x;
        t += every_ns;
    }
}

int main(void) {
    printf("=== pointer: how far the pointer goes for how far the hand went ===\n");
    int mono = 1;
    for (uint32_t s = 0; s < 4000 && mono; s++) mono = ptr_accel_gain(s + 1) >= ptr_accel_gain(s);
    CHECK(mono, "the gain never falls as the hand speeds up");
    CHECK(ptr_accel_gain(0) == PTR_ACCEL_ONE && ptr_accel_gain(153) == PTR_ACCEL_ONE, "aiming (under 0.6 counts/ms) is 1:1");
    CHECK(ptr_accel_gain(614) == 2 * PTR_ACCEL_ONE && ptr_accel_gain(100000) == 3 * PTR_ACCEL_ONE,
          "2x at 2.4 counts/ms, 3x at a sweep and never more");

    int32_t sx, sy;
    run(100, 1, -1, 8 * MS, 1000, &sx, &sy);           /* 0.125 counts/ms: aiming */
    CHECK(sx == 100 && sy == -100, "a slow aim moves exactly as far as the hand: 100 counts, 100 px");
    run(50, 16, 0, 8 * MS, 1000, &sx, &sy);            /* 2 counts/ms */
    printf("        (800 counts at 2 counts/ms went %d px)\n", sx);
    CHECK(sx > 1300 && sx < 1700, "a brisk stroke goes about 1.8 times as far");
    run(50, 60, 0, 8 * MS, 1000, &sx, &sy);            /* 7.5 counts/ms */
    CHECK(g_last_x == 180, "a sweep goes three times as far, exactly");
    /* 0.75 counts/ms: a gain of 277/256, 1.08 -- 299 reports of it after the
     * first, which is 1:1 */
    run(300, 3, 0, 4 * MS, 1000, &sx, &sy);
    int32_t want = (299 * 3 * 277 + 3 * 256) / 256;
    printf("        (900 counts at 0.75 counts/ms went %d px; %d owed exactly)\n", sx, want);
    CHECK(sx >= want - 1 && sx <= want, "a slow accelerated drift loses no more than the last fraction to rounding");
    run(50, 16, 0, 8 * MS, 500, &sx, &sy);
    int32_t half = g_last_x;
    run(50, 16, 0, 8 * MS, 1000, &sx, &sy);
    printf("        (a 16-count report at tracking 0.5: %d px, at 1.0: %d px)\n", half, g_last_x);
    CHECK(half * 2 >= g_last_x - 1 && half * 2 <= g_last_x + 1, "the tracking speed scales the curve: half is half");

    /* the first report after a pause is aiming, however large */
    struct ptr_accel a = { 0, 0, 0 };
    int32_t x = 3, y = 0;
    ptr_accel_apply(&a, &x, &y, 5000 * MS, 1000);
    CHECK(x == 3, "the first report of a motion is taken as it is");
    x = -2; y = 0;
    a.rem_x = 200;
    ptr_accel_apply(&a, &x, &y, 5100 * MS, 1000);
    CHECK(x == -2, "turning round drops the fraction owed the other way");

    printf("=== pointer: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
