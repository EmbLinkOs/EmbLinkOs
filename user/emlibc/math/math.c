/* math.c — emlibc's math GLUE over lifted fdlibm (user/emlibc/math/fdlibm/).
 *
 * The transcendentals are now REAL fdlibm (Sun's freely-licensed, ~1-ulp
 * correctly-rounded library, the same code newlib's libm is built from): the
 * fdlibm/ tree defines sin/cos/tan/atan/floor/ceil/fabs/frexp/ldexp/scalbn/
 * copysign/trunc/cbrt/expm1/tanh directly and the __ieee754_* cores for the
 * rest. This file is only the thin layer emlibc's <math.h> needs on top:
 *   - public wrappers naming the __ieee754_* cores (no matherr machinery),
 *   - hardware sqrt (SSE2), used by fdlibm's asin/acos/hypot too,
 *   - the two functions fdlibm's classic set does not carry (log2/exp2 by
 *     identity, round/modf over fdlibm's floor/trunc),
 *   - IEEE-754 classification and the float wrappers.
 * Compiled WITH SSE (userspace FP; the kernel saves it). */

#include <math.h>
#include <stdint.h>

/* fdlibm cores (defined in fdlibm/e_*.c). */
extern double __ieee754_exp(double), __ieee754_log(double), __ieee754_log10(double);
extern double __ieee754_pow(double, double), __ieee754_asin(double), __ieee754_acos(double);
extern double __ieee754_atan2(double, double), __ieee754_sinh(double), __ieee754_cosh(double);
extern double __ieee754_fmod(double, double), __ieee754_hypot(double, double);
/* (floor/trunc/copysign/sin/cos/tan/atan/ceil/fabs come from <math.h>, which
 * fdlibm/ defines and this glue only reuses.) */

typedef union { double d; uint64_t u; } di_t;

/* ---- classification (emlibc's <math.h> declares these) ---- */
int __em_isnan(double x)    { di_t v; v.d = x; return ((v.u >> 52) & 0x7ff) == 0x7ff && (v.u & 0xfffffffffffffULL) != 0; }
int __em_isinf(double x)    { di_t v; v.d = x; return (v.u & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL; }
int __em_isfinite(double x) { di_t v; v.d = x; return ((v.u >> 52) & 0x7ff) != 0x7ff; }
int __em_signbit(double x)  { di_t v; v.d = x; return (int)(v.u >> 63); }

/* ---- hardware sqrt (exact); __ieee754_sqrt is what fdlibm's asin/acos/hypot call ---- */
double sqrt(double x)         { double r; __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
double __ieee754_sqrt(double x){ return sqrt(x); }
float  sqrtf(float x)         { float  r; __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

/* ---- public wrappers over the fdlibm cores (no matherr) ---- */
double exp(double x)             { return __ieee754_exp(x); }
double log(double x)             { return __ieee754_log(x); }
double log10(double x)           { return __ieee754_log10(x); }
double pow(double x, double y)   { return __ieee754_pow(x, y); }
double asin(double x)            { return __ieee754_asin(x); }
double acos(double x)            { return __ieee754_acos(x); }
double atan2(double y, double x) { return __ieee754_atan2(y, x); }
double sinh(double x)            { return __ieee754_sinh(x); }
double cosh(double x)            { return __ieee754_cosh(x); }
double fmod(double x, double y)  { return __ieee754_fmod(x, y); }
double hypot(double x, double y) { return __ieee754_hypot(x, y); }

/* ---- the two fdlibm's classic set doesn't carry, by identity ---- */
double log2(double x)  { return __ieee754_log(x) * M_LOG2E; }
double exp2(double x)  { return __ieee754_exp(x * M_LN2); }
double round(double x) { double a = __em_signbit(x) ? -x : x;   /* half away from zero */
                         return copysign(floor(a + 0.5), x); }
double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }

/* ---- float wrappers (double math is amply precise for float) ---- */
float fabsf(float x)            { return (float)fabs(x); }
float floorf(float x)           { return (float)floor(x); }
float ceilf(float x)            { return (float)ceil(x); }
float expf(float x)             { return (float)__ieee754_exp(x); }
float logf(float x)             { return (float)__ieee754_log(x); }
float powf(float x, float y)    { return (float)__ieee754_pow(x, y); }
float sinf(float x)             { return (float)sin(x); }
float cosf(float x)             { return (float)cos(x); }
float tanf(float x)             { return (float)tan(x); }
float atan2f(float y, float x)  { return (float)__ieee754_atan2(y, x); }
