/* math.h — emlibc. The OS-agnostic transcendentals (docs/EMLIBC_Requirements.md
 * §4). This header is the CONTRACT; the implementation is deliberately
 * pragmatic (standard range reduction + polynomial approximations, accurate to
 * ~1e-9 on common ranges), NOT correctly-rounded to 0.5 ulp. The spec's
 * production path is to LIFT a vetted libm (musl / FreeBSD) behind this same
 * header -- authoring correctly-rounded transcendentals is the trap (D-006).
 * Shipping a working interim now is the incremental move; the header stays. */
#ifndef _EMLIBC_MATH_H
#define _EMLIBC_MATH_H

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880

#define HUGE_VAL   (__builtin_huge_val())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

/* Classification. Self-contained (no <fenv.h>); bit tests on the IEEE-754
 * double, which is what the target uses. */
int  __em_isnan(double x);
int  __em_isinf(double x);
int  __em_isfinite(double x);
int  __em_signbit(double x);
#define isnan(x)     __em_isnan((double)(x))
#define isinf(x)     __em_isinf((double)(x))
#define isfinite(x)  __em_isfinite((double)(x))
#define signbit(x)   __em_signbit((double)(x))

/* double */
double fabs(double x);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double copysign(double x, double y);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double ldexp(double x, int e);
double scalbn(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* float — thin wrappers over the double versions (accuracy is ample). */
float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);

#endif /* _EMLIBC_MATH_H */
