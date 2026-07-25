/* math.c — emlibc. The agnostic transcendentals (EMLIBC_Requirements.md §4).
 *
 * Exact where the hardware gives it (sqrt via SSE2 sqrtsd; fabs/floor/trunc via
 * IEEE-754 bit ops). Pragmatic elsewhere: standard range reduction + polynomial
 * approximations, accurate to roughly 1e-9 over common ranges -- NOT
 * correctly-rounded. The production path (spec, D-006) is to LIFT a vetted libm
 * behind <math.h>; authoring correctly-rounded transcendentals is the trap.
 * This is the honest interim: real, working, upgradeable without touching a
 * caller. Compiled WITH SSE (userspace FP; the kernel saves it). */

#include <math.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } di_t;

/* ---- classification (bit tests on the IEEE-754 double) ---- */
int __em_isnan(double x)    { di_t v; v.d = x; return ((v.u >> 52) & 0x7ff) == 0x7ff && (v.u & 0xfffffffffffffULL) != 0; }
int __em_isinf(double x)    { di_t v; v.d = x; return (v.u & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL; }
int __em_isfinite(double x) { di_t v; v.d = x; return ((v.u >> 52) & 0x7ff) != 0x7ff; }
int __em_signbit(double x)  { di_t v; v.d = x; return (int)(v.u >> 63); }

/* ---- exact: sign / sqrt / integer rounding ---- */
double fabs(double x)               { di_t v; v.d = x; v.u &= 0x7fffffffffffffffULL; return v.d; }
double copysign(double x, double y) { di_t a, b; a.d = x; b.d = y;
    a.u = (a.u & 0x7fffffffffffffffULL) | (b.u & 0x8000000000000000ULL); return a.d; }

double sqrt(double x)  { double r; __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float  sqrtf(float x)  { float  r; __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

double trunc(double x)
{
    if (!__em_isfinite(x)) return x;
    di_t v; v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    if (e < 0)   return copysign(0.0, x);
    if (e >= 52) return x;                       /* already integral */
    uint64_t mask = (1ULL << (52 - e)) - 1;
    v.u &= ~mask;
    return v.d;
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
double ceil(double x)  { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double round(double x) { return copysign(floor(fabs(x) + 0.5), x); }  /* half away from 0 */

/* ---- exponent surgery ---- */
double scalbn(double x, int e)
{
    if (x == 0.0 || !__em_isfinite(x)) return x;
    /* chunk e into the normal exponent range, then one exact multiply */
    while (e >  1023) { di_t t; t.u = (uint64_t)(1023 + 1023) << 52; x *= t.d; e -= 1023; }
    while (e < -1022) { di_t t; t.u = (uint64_t)(-1022 + 1023) << 52; x *= t.d; e += 1022; }
    di_t p; p.u = (uint64_t)(e + 1023) << 52;
    return x * p.d;
}
double ldexp(double x, int e) { return scalbn(x, e); }

double frexp(double x, int *e)
{
    if (x == 0.0 || !__em_isfinite(x)) { *e = 0; return x; }
    di_t v; v.d = x;
    *e = (int)((v.u >> 52) & 0x7ff) - 1022;        /* m in [0.5, 1) */
    v.u = (v.u & ~(0x7ffULL << 52)) | (1022ULL << 52);
    return v.d;
}
double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }
double fmod(double x, double y)
{
    if (y == 0.0 || __em_isnan(x) || __em_isnan(y) || __em_isinf(x)) return NAN;
    return x - trunc(x / y) * y;                   /* pragmatic; loses ulps for huge x/y */
}

/* ---- exp / log ---- */
double exp(double x)
{
    if (__em_isnan(x)) return x;
    if (x >  709.78)  return HUGE_VAL;
    if (x < -745.13)  return 0.0;
    double k = round(x * M_LOG2E);
    double r = x - k * M_LN2;                       /* r in [-ln2/2, ln2/2] */
    double p = 1.0 + r*(1.0 + r*(0.5 + r*((1.0/6) + r*((1.0/24) + r*((1.0/120)
               + r*((1.0/720) + r*(1.0/5040)))))));
    return scalbn(p, (int)k);
}
double exp2(double x) { return exp(x * M_LN2); }

double log(double x)
{
    if (__em_isnan(x) || x < 0.0) return NAN;
    if (x == 0.0)     return -HUGE_VAL;
    if (__em_isinf(x)) return x;
    int e; double m = frexp(x, &e);                 /* m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2.0; e -= 1; }   /* -> [sqrt(.5), sqrt(2)) */
    double s = (m - 1.0) / (m + 1.0), s2 = s * s;   /* log(m) = 2 atanh(s) */
    double t = s * (2.0 + s2*((2.0/3) + s2*((2.0/5) + s2*((2.0/7) + s2*((2.0/9) + s2*(2.0/11))))));
    return e * M_LN2 + t;
}
double log2(double x)  { return log(x) * M_LOG2E; }
double log10(double x) { return log(x) * (1.0 / M_LN10); }

double pow(double x, double y)
{
    if (y == 0.0 || x == 1.0) return 1.0;
    if (__em_isnan(x) || __em_isnan(y)) return NAN;
    if (x == 0.0) return (y > 0.0) ? 0.0 : HUGE_VAL;
    if (x < 0.0) {
        double ry = round(y);
        if (ry != y) return NAN;                    /* non-integer power of negative */
        double r = exp(y * log(-x));
        return ((long long)ry & 1) ? -r : r;
    }
    return exp(y * log(x));
}

/* ---- trig: reduce to [-pi/4, pi/4], track the quadrant ---- */
static double sin_poly(double r)
{ double r2 = r*r; return r*(1.0 + r2*(-(1.0/6) + r2*((1.0/120) + r2*(-(1.0/5040) + r2*(1.0/362880))))); }
static double cos_poly(double r)
{ double r2 = r*r; return 1.0 + r2*(-0.5 + r2*((1.0/24) + r2*(-(1.0/720) + r2*((1.0/40320) + r2*(-(1.0/3628800)))))); }

double sin(double x)
{
    if (!__em_isfinite(x)) return NAN;
    double k = round(x * (2.0 / M_PI));
    double r = x - k * M_PI_2;
    switch ((int)((long long)k & 3)) {
    case 0:  return  sin_poly(r);
    case 1:  return  cos_poly(r);
    case 2:  return -sin_poly(r);
    default: return -cos_poly(r);
    }
}
double cos(double x)
{
    if (!__em_isfinite(x)) return NAN;
    double k = round(x * (2.0 / M_PI));
    double r = x - k * M_PI_2;
    switch ((int)((long long)k & 3)) {
    case 0:  return  cos_poly(r);
    case 1:  return -sin_poly(r);
    case 2:  return -cos_poly(r);
    default: return  sin_poly(r);
    }
}
double tan(double x) { return sin(x) / cos(x); }

/* ---- inverse trig ---- */
double atan(double x)
{
    if (__em_isnan(x)) return x;
    int sign = 0, inv = 0;
    if (x < 0.0) { sign = 1; x = -x; }
    if (x > 1.0) { inv = 1; x = 1.0 / x; }
    double x2 = x * x;
    double a = x*(0.9999993329 + x2*(-0.3332985605 + x2*(0.1994653599 + x2*(-0.1390853351
               + x2*(0.0964200441 + x2*(-0.0559098861 + x2*(0.0218612288 - x2*0.0040540580)))))));
    if (inv)  a = M_PI_2 - a;
    return sign ? -a : a;
}
double atan2(double y, double x)
{
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return (y >= 0.0) ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    if (y > 0.0) return  M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;
}
double asin(double x) { if (x < -1.0 || x > 1.0) return NAN; return atan2(x, sqrt((1.0 - x)*(1.0 + x))); }
double acos(double x) { if (x < -1.0 || x > 1.0) return NAN; return atan2(sqrt((1.0 - x)*(1.0 + x)), x); }

/* ---- hyperbolic ---- */
double sinh(double x) { double e = exp(x); return (e - 1.0/e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0/e) * 0.5; }
double tanh(double x) { if (x > 20.0) return 1.0; if (x < -20.0) return -1.0;
                        double e = exp(2.0 * x); return (e - 1.0) / (e + 1.0); }

/* ---- misc ---- */
double hypot(double x, double y)
{
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r*r);
}
double cbrt(double x)
{
    if (x == 0.0) return 0.0;
    int neg = x < 0.0; if (neg) x = -x;
    double r = exp(log(x) / 3.0);
    r = r - (r - x/(r*r)) / 3.0;                    /* one Newton refinement */
    return neg ? -r : r;
}

/* ---- float wrappers (double math is amply precise for float) ---- */
float fabsf(float x)            { return (float)fabs(x); }
float floorf(float x)           { return (float)floor(x); }
float ceilf(float x)            { return (float)ceil(x); }
float expf(float x)             { return (float)exp(x); }
float logf(float x)             { return (float)log(x); }
float powf(float x, float y)    { return (float)pow(x, y); }
float sinf(float x)             { return (float)sin(x); }
float cosf(float x)             { return (float)cos(x); }
float tanf(float x)             { return (float)tan(x); }
float atan2f(float y, float x)  { return (float)atan2(y, x); }
