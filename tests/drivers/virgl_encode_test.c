/* virgl_encode_test.c -- the virgl stream encoder on its own, on any host:
 * the integer-only floats every coordinate and colour goes through, checked
 * bit for bit against the C library's correctly rounded division, and the
 * layouts and overflow rule of the commands. The shaders and what they draw
 * are checked on a real renderer by virgl_compose_test.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/video/virgl.h"

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* num/den rounded once, to a float. Through long double where it has the
 * width to make that exact (x86's 64-bit mantissa); else from a double, which
 * is exact for these operands below 2^24. */
static uint32_t want(long long num, long long den) { return bits((float)((long double)num / (long double)den)); }

static int failures;
static void check(long long num, long long den)
{
    uint32_t got = vg_f32_ratio(num, den), w = want(num, den);
    if (got != w) {
        if (failures++ < 10) printf("vg_f32_ratio(%lld, %lld) = %08x, want %08x\n", num, den, got, w);
    }
}

int main(void)
{
    /* Everything the compositor makes: pixels, halves, 1/n, 2/W, colours /255. */
    for (long long n = -4100; n <= 4100; n++)
        for (long long d = 1; d <= 300; d += (d < 20 ? 1 : 37)) check(n, d);
    for (long long d = 1; d <= 16384; d++) { check(2, d); check(-2, d); check(1, d); check(2 * d + 1, 2); check(2 * d - 1, 2); }
    for (long long c = 0; c <= 255; c++) check(c, 255);
    /* Ties to even, exactly at half a unit: 2^24 + 1 and + 3 have one bit too many. */
    check((1LL << 24) + 1, 1); check((1LL << 24) + 3, 1); check((1LL << 25) + 2, 1); check((1LL << 25) + 6, 1);
    /* Wide operands: the normalisation, the carry out of bit 63 in the long division. */
    unsigned long long x = 88172645463325252ULL;
    for (int i = 0; i < 200000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        long long n = (long long)(x >> 1), d = (long long)((x * 0x9e3779b97f4a7c15ULL) >> 1) | 1;
        if (i & 1) n >>= (x & 31);
        if (i & 2) d >>= ((x >> 8) & 31);
        if (!d) d = 1;
        if ((long double)n / (long double)d < 1e-30L || (long double)n / (long double)d > 1e30L) continue;
        check(n, d);
        check(-n, d);
    }
    check(0x7fffffffffffffffLL, 1); check(1, 0x7fffffffffffffffLL); check(0x7fffffffffffffffLL, 0x7ffffffffffffffeLL);
    assert(vg_f32_ratio(0, 5) == 0 && vg_f32_ratio(5, 0) == 0);
    assert(vg_f32(1) == 0x3f800000u && vg_f32(-1) == 0xbf800000u && vg_f32_ratio(1, 2) == 0x3f000000u);
    if (failures) { printf("vg_f32_ratio: %d wrong\n", failures); return 1; }

    /* A stream that runs out of room says so and keeps nothing half-written. */
    uint32_t buf[8];
    struct vg_stream s = { buf, 0, 8, 0 };
    vg_scissor(&s, 1, 2, 3, 4);                      /* 4 dwords */
    assert(s.n == 4 && !s.overflow);
    assert(buf[0] == (15u | (3u << 16)) && buf[1] == 0 && buf[2] == (1u | (2u << 16)) && buf[3] == (3u | (4u << 16)));
    vg_viewport(&s, 640, 480, 0);                    /* 8 more: does not fit */
    assert(s.overflow && s.n == 4);
    assert(!vg_room(&s, 1));

    /* The viewport maps clip space back to pixels: scale (w/2, h/2 or -h/2), translate (w/2, h/2). */
    uint32_t vb[8];
    struct vg_stream v = { vb, 0, 8, 0 };
    vg_viewport(&v, 640, 480, 1);
    assert(vb[0] == (4u | (7u << 16)) && vb[2] == bits(320.0f) && vb[3] == bits(-240.0f) &&
           vb[4] == bits(0.5f) && vb[5] == bits(320.0f) && vb[6] == bits(240.0f) && vb[7] == bits(0.5f));

    /* A shader carries its text with the NUL, padded to whole dwords. */
    uint32_t sb[32];
    struct vg_stream sh = { sb, 0, 32, 0 };
    vg_shader(&sh, 7, 1, "FRAG\nEND\n");
    assert((sb[0] & 0xff) == 1 && ((sb[0] >> 8) & 0xff) == 4 && sb[1] == 7 && sb[2] == 1);
    assert(!memcmp((const char *)&sb[sh.n - 3], "FRAG\nEND\n", 10));
    puts("virgl encode: integer floats exact against the C library (ties to even, wide operands), "
         "command layouts, overflow refused whole");
    return 0;
}
