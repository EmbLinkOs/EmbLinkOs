/* emlibc_math.c -- exercises emlibc's <math.h> (and %f/%e in its printf).
 * Linked against emlibc, not newlib. It checks a spread of functions against
 * known values within tolerance, writes a formatted witness to disk (the
 * serial-checkable proof), and exits 42 iff every check passed.
 */
#include <math.h>
#include <stdio.h>

/* Tight RELATIVE tolerance: real fdlibm is ~1 ulp, so 1e-12 passes easily. The
 * old pragmatic math.c (~1e-9, naive range reduction) would FAIL these --
 * especially sin(1e8), where only fdlibm's exact rem_pio2 reduction survives. */
static int near(double a, double b)
{
    double d = a - b; if (d < 0) d = -d;
    double m = b < 0 ? -b : b; if (m < 1.0) m = 1.0;
    return d <= 1e-12 * m;
}

int main(void)
{
    int ok = 1;
    ok &= near(sqrt(2.0),        1.4142135623730951);
    ok &= near(fabs(-3.5),       3.5);
    ok &= near(floor(3.7),       3.0);
    ok &= near(ceil(3.2),        4.0);
    ok &= near(fmod(10.0, 3.0),  1.0);
    ok &= near(sin(M_PI / 6.0),  0.5);
    ok &= near(cos(0.0),         1.0);
    ok &= near(tan(M_PI / 4.0),  1.0);
    ok &= near(exp(1.0),         2.718281828459045);
    ok &= near(log(2.0),         0.6931471805599453);
    ok &= near(log10(1000.0),    3.0);
    ok &= near(pow(2.0, 0.5),    1.4142135623730951);
    ok &= near(atan2(1.0, 1.0),  M_PI_4);
    ok &= near(hypot(3.0, 4.0),  5.0);
    ok &= near(asin(1.0),        M_PI_2);
    ok &= near(sin(1e8),         0.93163902710972601);  /* large arg: needs exact rem_pio2 */
    ok &= near(cos(1000.0),      0.56237907629070294);
    ok &= near(exp(20.0),        485165195.40979028);

    FILE *f = fopen("/data/tmp/math.out", "w");
    if (f) {
        /* also exercises the formatter: %g (pi), %e (big), field width (pad) */
        fprintf(f, "sqrt2=%.6f pi=%g big=%.2e pad=[%8.2f] neg=%+.1f pass=%d\n",
                sqrt(2.0), M_PI, 6.022e23, 3.14159, -2.5, ok);
        fclose(f);
    }

    printf("emlibc math: sqrt2=%.9f  6.022e23=%.3e  pi=%g  [%10.3f]  ok=%d\n",
           sqrt(2.0), 6.022e23, M_PI, 2.5, ok);
    return ok ? 42 : 1;
}
