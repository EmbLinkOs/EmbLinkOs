/* spring_test.c -- the desktop's springs and velocity fit (ui/motion/) on the
 * host: one trajectory at any frame rate, the overshoot and settle a response
 * and damping promise, motion that stays continuous when retargeted, and the
 * velocity a hand let go with.  make spring-test -> exits 0 iff every claim holds. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "motion/spring.h"
#include "motion/velocity.h"

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

#define MS 1000000ull
#define T0 (1000 * MS)                          /* the clock does not start at 0 */

static double px(const struct spring *s) { return (double)s->x / SPRING_ONE; }

/* A spring from 0 toward `to`, stepped by frames `frame_ns(i)` apart, sampled
 * every 10 ms into `out` (n samples). */
static void run(int64_t to, uint32_t resp, uint32_t damp, uint64_t (*frame_ns)(unsigned), double *out, int n) {
    struct spring s;
    spring_init(&s, 0, T0);
    spring_to(&s, SPRING_PX(to), spring_params(resp, damp), T0);
    uint64_t t = T0;
    unsigned i = 0;
    for (int k = 1; k <= n; k++) {
        uint64_t sample = T0 + (uint64_t)k * 10 * MS;
        while (t + frame_ns(i) < sample) { t += frame_ns(i++); spring_step(&s, t); }
        spring_step(&s, sample);
        out[k - 1] = px(&s);
    }
}
static uint64_t at60(unsigned i) { (void)i; return 16666667; }
static uint64_t at120(unsigned i) { (void)i; return 8333333; }
static uint64_t ragged(unsigned i) { return (3 + (i * 7919u) % 23u) * MS + (i * 104729u) % 1000000u; }

int main(void) {
    printf("=== spring: one motion at any frame rate ===\n");

    /* THE TOKENS: k and c as the formulas give them */
    struct spring_params p = spring_params(SPRING_SMOOTH);
    double w = 2 * M_PI / 0.350;
    CHECK(fabs((double)p.k / SPRING_ONE - w * w) / (w * w) < 0.001, "smooth: k = (2 pi / 0.35 s)^2, within 0.1%");
    CHECK(fabs((double)p.c / SPRING_ONE - 2 * w) / (2 * w) < 0.001, "smooth: c = 4 pi * 1.0 / 0.35 s, within 0.1%");

    /* ONE TRAJECTORY whatever asks for it */
    enum { N = 100 };
    double a[N], b[N], c[N];
    run(1000, SPRING_SMOOTH, at60, a, N);
    run(1000, SPRING_SMOOTH, at120, b, N);
    run(1000, SPRING_SMOOTH, ragged, c, N);
    int same = 1;
    for (int i = 0; i < N; i++) same &= a[i] == b[i] && b[i] == c[i];
    CHECK(same, "60 Hz, 120 Hz and ragged frames (3-26 ms) see the same positions, exactly");

    /* AGAINST THE EXACT SOLUTION: critically damped, x = D (1 - (1 + w t) e^(-w t)) */
    double worst = 0;
    for (int i = 0; i < N; i++) {
        double t = (i + 1) * 0.010, exact = 1000 * (1 - (1 + w * t) * exp(-w * t));
        if (fabs(a[i] - exact) > worst) worst = fabs(a[i] - exact);
    }
    printf("        (worst distance from the exact curve over 1000 px: %.2f px)\n", worst);
    /* First-order (semi-implicit Euler): it trails the exact curve a little,
     * about 2 ms at its fastest -- a lag no one sees, for an integrator that
     * cannot gain energy. */
    CHECK(worst < 10.0, "smooth: within 1% of the exact curve over a 1000 px move");
    double peak = 0;
    for (int i = 0; i < N; i++) if (a[i] > peak) peak = a[i];
    CHECK(peak <= 1000.0, "smooth (damping 1): never past the target");
    CHECK(a[34] > 980.0, "smooth: 98% of the way at its response, 350 ms");
    CHECK(a[N - 1] == 1000.0, "smooth: at rest, exactly on the target, after a second");

    /* UNDERDAMPED: the overshoot exp(-zeta pi / sqrt(1 - zeta^2)), 4.6% at 0.7 */
    run(1000, SPRING_BOUNCY, at120, b, N);
    peak = 0;
    for (int i = 0; i < N; i++) if (b[i] > peak) peak = b[i];
    printf("        (bouncy overshoots to %.1f px)\n", peak);
    CHECK(peak > 1040.0 && peak < 1052.0, "bouncy (damping 0.7): overshoots 4-5%, as the formula says");

    /* INTERRUPTED: turned around mid-flight, from where it is at its speed */
    struct spring s;
    spring_init(&s, 0, T0);
    spring_to(&s, SPRING_PX(800), spring_params(SPRING_SMOOTH), T0);
    spring_step(&s, T0 + 120 * MS);
    int64_t x0 = s.x, v0 = s.v;
    spring_to(&s, SPRING_PX(0), spring_params(SPRING_SNAPPY), T0 + 120 * MS);
    CHECK(s.x == x0 && s.v == v0 && v0 > 0, "retargeted: position and velocity carry over unchanged");
    spring_step(&s, T0 + 130 * MS);
    CHECK(s.x > x0, "retargeted: it keeps going the way it was moving for a moment, then turns");
    int moving = 1;
    for (uint64_t t = 130; t < 2000 && moving; t += 8) moving = spring_step(&s, T0 + t * MS);
    CHECK(!moving && s.x == 0 && s.v == 0, "retargeted: comes to rest exactly on the new target");

    /* THROWN: a velocity from rest, out and back */
    spring_init(&s, SPRING_PX(500), T0);
    spring_kick(&s, SPRING_PX(3000), T0);
    spring_step(&s, T0 + 50 * MS);
    CHECK(px(&s) > 540.0, "thrown at 3000 px/s: it leaves the target");
    for (uint64_t t = 50; t < 2000 && spring_step(&s, T0 + t * MS); t += 16) {}
    CHECK(s.x == SPRING_PX(500) && !s.moving, "thrown: it comes back and rests on the target");

    /* A STALL: a frame five seconds late finds the motion over, not replayed */
    spring_init(&s, 0, T0);
    spring_to(&s, SPRING_PX(300), spring_params(SPRING_SMOOTH), T0);
    CHECK(spring_step(&s, T0 + 5000 * MS) == 0 && s.x == SPRING_PX(300), "a 5 s stall: at rest on the target");
    CHECK(spring_px(&s) == 300, "spring_px: whole pixels");

    printf("=== velocity: what the hand let go with ===\n");
    struct vel_track v;
    int32_t vx, vy;
    vel_reset(&v);
    uint64_t t = T0;
    srand(7);
    for (int i = 0; i < 40; i++) {          /* 1000 px/s right, -400 px/s up, reports every 6-10 ms */
        t += (6 + rand() % 5) * MS;
        double sec = (double)(t - T0) / 1e9;
        vel_add(&v, t, (int32_t)floor(1000 * sec), (int32_t)floor(-400 * sec));
    }
    vel_get(&v, t, &vx, &vy);
    printf("        (fitted %d, %d px/s)\n", vx, vy);
    CHECK(abs(vx - 1000) <= 30 && abs(vy + 400) <= 30, "uneven, whole-pixel reports: the fit is within 3%");
    vel_get(&v, t + 120 * MS, &vx, &vy);
    CHECK(vx == 0 && vy == 0, "a hand that held still 120 ms before letting go throws nothing");
    vel_reset(&v);
    vel_add(&v, T0, 10, 10);
    vel_get(&v, T0, &vx, &vy);
    CHECK(vx == 0 && vy == 0, "one sample is no velocity");
    vel_add(&v, T0 + 10 * MS, 30, 10);
    vel_get(&v, T0 + 10 * MS, &vx, &vy);
    CHECK(vx == 2000 && vy == 0, "two samples: their slope (20 px in 10 ms is 2000 px/s)");

    printf("=== spring: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
