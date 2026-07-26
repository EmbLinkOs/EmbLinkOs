/* math_config.h — emlibc's minimal shim for the classic fdlibm cores. Newlib's
 * real math_config.h is a large file for its NEW (ARM optimized-routines) math;
 * the classic e_exp.c/e_pow.c/e_cosh.c/s_expm1.c we lift use only two symbols
 * from it -- the overflow/underflow result helpers. Provide exactly those. */
#ifndef _EMLIBC_FDLIBM_MATH_CONFIG_H
#define _EMLIBC_FDLIBM_MATH_CONFIG_H

#include <math.h>
#include <errno.h>
#include <stdint.h>

/* sign==0 -> +overflow (+Inf / +0), sign!=0 -> negative. errno=ERANGE, as a
 * libm should. The multiply forces the IEEE overflow/underflow flag the way
 * newlib's helpers do; the returned value is what fdlibm's callers expect. */
static inline double __math_oflow(uint32_t sign)
{
    errno = ERANGE;
    return (sign ? -1.0 : 1.0) * HUGE_VAL;
}
static inline double __math_uflow(uint32_t sign)
{
    errno = ERANGE;
    return sign ? -0.0 : 0.0;
}

/* Signaling-NaN test newlib's e_pow.c reaches for: a NaN whose quiet bit
 * (mantissa MSB) is clear. */
static inline int issignaling_inline(double x)
{
    union { double d; uint64_t u; } v; v.d = x;
    uint64_t man = v.u & 0xfffffffffffffULL;
    return ((v.u >> 52) & 0x7ff) == 0x7ff && man != 0 && !(man & 0x8000000000000ULL);
}

#endif /* _EMLIBC_FDLIBM_MATH_CONFIG_H */
