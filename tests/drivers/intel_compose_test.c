/* intel_compose_test.c -- the desktop composed with the Gen9 blitter, on a
 * model of the engine (intel_blt_model.h).
 *
 * Bring-up: forcewake taken and given back, the private PAT, execlists on,
 * the status page placed, the probe blit read back. Then every pixel check
 * virgl answers to (compose_checks.h), now drawn by blits and the CPU into
 * a composition buffer and presented onto a scanout the model marks as
 * display memory -- which must only ever be written uncached. Then what a
 * blitter brings of its own: the ring wrapping thousands of commands over,
 * a TLB that must be invalidated when a graphics address gets new pages,
 * the scanout holding exactly the composed frame, forcewake dropped when the
 * engine is idle, and an engine that hangs: found, reset, given up on. */
#include <stdarg.h>
#include "intel_blt_model.h"
#include "drivers/video/intel_compose.h"

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (getenv("INTEL_VERBOSE")) vprintf(fmt, ap);
    va_end(ap);
}

#define W 320
#define H 200
static uint32_t screen[W * H];          /* the framebuffer's draw buffer: where reads land */
static int failures;
/* Image pixels in the model's memory: the engine reads them where they are. */
static uint32_t *check_pixels(size_t n) { uint32_t *p = m_alloc(n * 4); assert(p); return p; }
#include "compose_checks.h"

#define FRONT_GGTT 0x00400000u
static uint64_t front_phys;

static int env_front(void *c, uint32_t *ggtt, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    (void)c;
    *ggtt = FRONT_GGTT; *pitch = W * 4; *w = W; *h = H;
    return 0;
}
static void *env_back(void *c, uint32_t *pitch) { (void)c; *pitch = W * 4; return screen; }
static void *env_alloc(void *c, uint64_t bytes, void **raw) { (void)c; return *raw = m_alloc(bytes); }
static void env_free(void *c, void *raw) { (void)c; m_free(raw); }

static uint32_t front_px(int x, int y) { return ((uint32_t *)m_ptr(front_phys))[y * W + x] & 0xffffffu; }

int main(void)
{
    /* The scanout: pages in the GGTT, as intel_native.c makes one. */
    uint32_t front_pages = (W * H * 4 + 4095) / 4096;
    front_phys = m_alloc_run(front_pages);
    for (uint32_t i = 0; i < front_pages; i++) {
        m_ggtt[FRONT_GGTT / 4096 + i] = (front_phys + (uint64_t)i * 4096) | 1u;
        m_display[(front_phys - M_PHYS_BASE) / 4096 + i] = 1;
    }

    /* 1. Bring-up. */
    struct intel_blt *b = intel_blt_init(&m_regs_ops, &m_mem_ops);
    CHECK(b != 0, "the blitter did not come up");
    if (!b) return 1;
    CHECK(m_submits >= 1 && !m_faults, "the probe did not run cleanly (%u submissions, %u faults)", m_submits, m_faults);
    CHECK(m_resets == 1, "bring-up resets the engine once (%u)", m_resets);
    CHECK(!(m_regs[FORCEWAKE_ACK_RENDER_GEN9 / 4] & 1u) && !(m_regs[FORCEWAKE_ACK_BLITTER_GEN9 / 4] & 1u),
          "forcewake still held when idle");
    CHECK(m_regs[RING_HWS_PGA(BCS_BASE) / 4] >= 0x10000000u, "status page not placed above the aperture");
    CHECK(!intel_blt_dead(b), "the blitter is dead after its probe");

    struct intel_compose_env env = { b, &m_mem_ops, env_front, env_back, env_alloc, env_free, 0 };
    assert(intel_compose_init(&env) == 0);
    const struct gpu_compose *ops = intel_compose();
    assert(ops && !strcmp(ops->name, "Intel blitter"));

    /* 2. Everything virgl draws, drawn the same. */
    compose_checks(ops);
    CHECK(!m_faults, "the engine model saw %u faults during the checks", m_faults);

    /* 3. The scanout holds the composed frame exactly. */
    struct gpu_rect all = { 0, 0, W, H };
    static uint32_t art[64 * 64];
    for (int i = 0; i < 64 * 64; i++) art[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
    uint32_t *art_shared = m_alloc(sizeof art);           /* window pixels live in pages the engine reaches */
    memcpy(art_shared, art, sizeof art);
    uint32_t art_id;
    assert(ops->image_create(art_shared, 64, 64, 256, &art_id) == 0);
    assert(ops->begin(&all) == 0);
    ops->fill(&all, 0x203040);
    struct gpu_draw win = { art_id, { 0, 0, 64, 64 }, { 30, 30, 64, 64 }, 1, 255, 8, 1, { 30, 30, 64, 64 }, 0x71b5aa };
    ops->draw(&win);
    struct gpu_rect sh = { 120, 40, 60, 40 };
    ops->shadow(&sh, &sh, 8, 12, 100);
    assert(ops->end() == 0);
    assert(ops->read(&all) == 0);
    int same = 1;
    for (int y = 0; y < H && same; y++)
        for (int x = 0; x < W && same; x++)
            if (front_px(x, y) != (screen[y * W + x] & 0xffffffu)) {
                same = 0;
                printf("  front (%d,%d) %06x, composed %06x\n", x, y, front_px(x, y), screen[y * W + x] & 0xffffffu);
            }
    CHECK(same, "the scanout is not the composed frame");
    CHECK(front_px(60, 60) == (art[30 * 64 + 30] & 0xffffffu), "window inside: %06x", front_px(60, 60));
    CHECK(front_px(30, 30) == 0x203040, "rounded corner not cut: %06x", front_px(30, 30));
    CHECK(front_px(60, 30) == 0x71b5aa && front_px(30, 60) == 0x71b5aa, "ring: %06x %06x", front_px(60, 30), front_px(30, 60));

    /* 4. The ring wraps: 3000 one-pixel fills, far more than 16 KiB of commands. */
    assert(ops->begin(&all) == 0);
    for (int i = 0; i < 3000; i++) {
        struct gpu_rect p = { i % W, (i / W) % H, 1, 1 };
        ops->fill(&p, (uint32_t)i * 0x010101u & 0xffffffu);
    }
    assert(ops->end() == 0);
    assert(ops->read(&all) == 0);
    int wrapped = 1;
    for (int i = 0; i < 3000 && wrapped; i++)
        if ((screen[((i / W) % H) * W + i % W] & 0xffffffu) != ((uint32_t)i * 0x010101u & 0xffffffu)) wrapped = 0;
    CHECK(wrapped, "fills lost or misplaced across ring wraps");

    /* 5. New pages under a graphics address the engine already used: only a
     *    TLB invalidation makes it draw the new ones. */
    uint32_t *red = m_alloc(64 * 64 * 4), *blue = m_alloc(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) { red[i] = 0xffff0000u; blue[i] = 0xff0000ffu; }
    uint32_t r_id, b_id;
    struct gpu_draw plain = { 0, { 0, 0, 64, 64 }, { 200, 100, 64, 64 }, 1, 255, 0, 0, { 0, 0, 0, 0 }, 0 };
    assert(ops->image_create(red, 64, 64, 256, &r_id) == 0);
    plain.image = r_id;
    assert(ops->begin(&all) == 0); ops->draw(&plain); assert(ops->end() == 0);
    ops->image_destroy(r_id);
    assert(ops->image_create(blue, 64, 64, 256, &b_id) == 0);
    plain.image = b_id;
    assert(ops->begin(&all) == 0); ops->draw(&plain); assert(ops->end() == 0);
    assert(ops->read(&all) == 0);
    CHECK((screen[120 * W + 220] & 0xffffffu) == 0x0000ff, "drawn from stale pages: %06x", screen[120 * W + 220] & 0xffffffu);
    CHECK(!(m_regs[FORCEWAKE_ACK_BLITTER_GEN9 / 4] & 1u), "forcewake held after the frames");
    CHECK(!m_faults, "the engine model saw %u faults", m_faults);

    /* 6. An engine that takes work and never finishes it: found, reset,
     *    given up on -- and composition says so instead of hanging. */
    unsigned resets = m_resets;
    m_hang = 1;
    assert(ops->begin(&all) == 0);
    ops->fill(&all, 0x112233);
    CHECK(ops->end() != 0, "a hung engine reported a frame shown");
    CHECK(m_resets == resets + 1, "the hung engine was not reset");
    CHECK(intel_blt_dead(b) && !intel_compose(), "composition still offered after the engine died");
    CHECK(!(m_regs[FORCEWAKE_ACK_BLITTER_GEN9 / 4] & 1u), "forcewake held by a dead engine");

    if (failures) { printf("intel compose: %d failures\n", failures); return 1; }
    puts("intel compose: blitter bring-up on the engine model, the compose checks virgl passes, the scanout "
         "written uncached and equal to the frame, ring wraps, TLB invalidation, forcewake released, a hang "
         "found and reset");
    return 0;
}
