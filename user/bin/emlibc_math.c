/* emlibc_math.c -- exercises emlibc's <math.h> (and %f/%e in its printf).
 * Linked against emlibc, not newlib. It checks a spread of functions against
 * known values within tolerance, writes a formatted witness to disk (the
 * serial-checkable proof), and exits 42 iff every check passed.
 */
#include <math.h>
#include <stdio.h>

static int near(double a, double b) { double d = a - b; if (d < 0) d = -d; return d < 1e-6; }

int main(void)
{
    int ok = 1;
    ok &= near(sqrt(2.0),        1.41421356237309515);
    ok &= near(fabs(-3.5),       3.5);
    ok &= near(floor(3.7),       3.0);
    ok &= near(ceil(3.2),        4.0);
    ok &= near(fmod(10.0, 3.0),  1.0);
    ok &= near(sin(M_PI / 6.0),  0.5);
    ok &= near(cos(0.0),         1.0);
    ok &= near(tan(M_PI / 4.0),  1.0);
    ok &= near(exp(1.0),         M_E);
    ok &= near(log(M_E),         1.0);
    ok &= near(log10(1000.0),    3.0);
    ok &= near(pow(2.0, 10.0),   1024.0);
    ok &= near(atan2(1.0, 1.0),  M_PI_4);
    ok &= near(hypot(3.0, 4.0),  5.0);
    ok &= near(asin(1.0),        M_PI_2);

    FILE *f = fopen("/data/tmp/math.out", "w");
    if (f) {
        fprintf(f, "sqrt2=%.6f sin30=%.4f e=%.6f 2^10=%.1f atan2=%.6f pass=%d\n",
                sqrt(2.0), sin(M_PI / 6.0), exp(1.0), pow(2.0, 10.0),
                atan2(1.0, 1.0), ok);
        fclose(f);
    }

    /* also prove %e works */
    printf("emlibc math: sqrt2=%.9f  6.022e23=%.3e  ok=%d\n",
           sqrt(2.0), 6.022e23, ok);
    return ok ? 42 : 1;
}
