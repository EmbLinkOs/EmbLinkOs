/* mathself.c -- proves EmbCC's own FP codegen computes real fdlibm correctly.
 * Uses only <math.h> + the raw open/write rim (no stdio), so its closure is
 * small: it checks a spread of functions at 1e-12 RELATIVE tolerance (real
 * fdlibm is ~1 ulp), writes a one-line witness, and exits 42 iff all pass.
 * When every object here -- crt0 + libc + fdlibm + this app -- is compiled by
 * EmbCC and linked by EmbLD on the OS, the own-toolchain loop closes INCLUDING
 * floating point. */
#include <math.h>
#include <unistd.h>

static int near(double a, double b)
{
    double d = a - b; if (d < 0) d = -d;
    double m = b < 0 ? -b : b; if (m < 1.0) m = 1.0;
    return d <= 1e-12 * m;
}

int main(void)
{
    int ok = 1;
    ok &= near(sqrt(2.0),      1.4142135623730951);
    ok &= near(sin(1e8),       0.93163902710972601);  /* exact rem_pio2 */
    ok &= near(cos(1000.0),    0.56237907629070294);
    ok &= near(exp(1.0),       2.718281828459045);
    ok &= near(log(2.0),       0.6931471805599453);
    ok &= near(pow(2.0, 0.5),  1.4142135623730951);
    ok &= near(atan2(1.0,1.0), M_PI_4);
    ok &= near(tanh(1.0),      0.7615941559557649);
    ok &= near(cbrt(27.0),     3.0);

    int fd = open("/data/tmp/mathself.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *m = ok ? "math self OK\n" : "math self FAIL\n";
        long n = 0; while (m[n]) n++;
        write(fd, m, (unsigned long)n);
        close(fd);
    }
    return ok ? 42 : 1;
}
